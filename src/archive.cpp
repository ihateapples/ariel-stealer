#include "archive.hpp"

#include "util.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <string_view>

namespace ariel::archive {

namespace {

std::array<uint32_t, 256> make_crc_table() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit)
            value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        table[i] = value;
    }
    return table;
}

uint32_t crc32(const std::vector<uint8_t>& data) {
    static const auto table = make_crc_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : data)
        crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFFu];
    return crc ^ 0xFFFFFFFFu;
}

class zip_crypto {
public:
    uint8_t encrypt(const uint8_t plaintext) {
        update(plaintext);
        const uint32_t temp = key2_ | 2u;
        return static_cast<uint8_t>(plaintext ^ static_cast<uint8_t>((temp * (temp ^ 1u)) >> 8));
    }

private:
    void update(const uint8_t byte) {
        static const auto table = make_crc_table();
        key0_ = (key0_ >> 8) ^ table[(key0_ ^ byte) & 0xFFu];
        key1_ = (key1_ >> 8) ^ table[(key1_ ^ (key0_ & 0xFFu)) & 0xFFu];
        key2_ = (key2_ >> 8) ^ table[(key2_ ^ (key1_ & 0xFFu)) & 0xFFu];
    }

    uint32_t key0_ = 0x12345678u;
    uint32_t key1_ = 0x23456789u;
    uint32_t key2_ = 0x34567890u;
};

void append_u16(std::vector<uint8_t>& out, const uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

void append_u32(std::vector<uint8_t>& out, const uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
}

void append_text(std::vector<uint8_t>& out, const std::string_view value) {
    out.insert(out.end(), value.begin(), value.end());
}

struct entry_meta {
    std::string name;
    uint16_t flags;
    uint16_t time;
    uint16_t date;
    uint32_t crc;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t offset;
};

}  // namespace

bool write_zip(const util::fs::path& path,
               const std::vector<entry>& entries,
               const std::string& password,
               std::string& error) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    const uint16_t dos_time = static_cast<uint16_t>((now.wHour << 11) | (now.wMinute << 5) |
                                                    (now.wSecond / 2));
    const uint16_t dos_date = static_cast<uint16_t>(((now.wYear >= 1980 ? now.wYear - 1980 : 0) << 9) |
                                                    (now.wMonth << 5) | now.wDay);
    const bool encrypted = !password.empty();

    std::vector<uint8_t> output;
    std::vector<entry_meta> directory;
    directory.reserve(entries.size());

    for (const auto& item : entries) {
        entry_meta meta{};
        meta.name = item.name;
        meta.flags = 0x0800;  // UTF-8 filenames
        if (encrypted) meta.flags |= 0x0001;
        meta.time = dos_time;
        meta.date = dos_date;
        meta.crc = crc32(item.data);
        meta.uncompressed_size = static_cast<uint32_t>(item.data.size());
        meta.compressed_size = meta.uncompressed_size + (encrypted ? 12u : 0u);
        meta.offset = static_cast<uint32_t>(output.size());

        append_u32(output, 0x04034B50u);
        append_u16(output, 20);
        append_u16(output, meta.flags);
        append_u16(output, 0);  // stored
        append_u16(output, meta.time);
        append_u16(output, meta.date);
        append_u32(output, meta.crc);
        append_u32(output, meta.compressed_size);
        append_u32(output, meta.uncompressed_size);
        append_u16(output, static_cast<uint16_t>(item.name.size()));
        append_u16(output, 0);
        append_text(output, item.name);

        if (encrypted) {
            uint8_t header[12]{};
            if (BCryptGenRandom(nullptr, header, 11, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
                error = "random source failed";
                return false;
            }
            header[11] = static_cast<uint8_t>((meta.crc >> 24) & 0xFFu);
            zip_crypto cipher;
            for (const uint8_t byte : header) output.push_back(cipher.encrypt(byte));
            for (const uint8_t byte : item.data) output.push_back(cipher.encrypt(byte));
        } else {
            output.insert(output.end(), item.data.begin(), item.data.end());
        }
        directory.push_back(meta);
    }

    const uint32_t directory_offset = static_cast<uint32_t>(output.size());
    for (const auto& meta : directory) {
        append_u32(output, 0x02014B50u);
        append_u16(output, 20);
        append_u16(output, 20);
        append_u16(output, meta.flags);
        append_u16(output, 0);
        append_u16(output, meta.time);
        append_u16(output, meta.date);
        append_u32(output, meta.crc);
        append_u32(output, meta.compressed_size);
        append_u32(output, meta.uncompressed_size);
        append_u16(output, static_cast<uint16_t>(meta.name.size()));
        append_u16(output, 0);
        append_u16(output, 0);
        append_u16(output, 0);
        append_u16(output, 0);
        append_u32(output, 0);
        append_u32(output, meta.offset);
        append_text(output, meta.name);
    }
    const uint32_t directory_size = static_cast<uint32_t>(output.size()) - directory_offset;

    append_u32(output, 0x06054B50u);
    append_u16(output, 0);
    append_u16(output, 0);
    append_u16(output, static_cast<uint16_t>(directory.size()));
    append_u16(output, static_cast<uint16_t>(directory.size()));
    append_u32(output, directory_size);
    append_u32(output, directory_offset);
    append_u16(output, 0);

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "cannot create archive";
        return false;
    }
    file.write(reinterpret_cast<const char*>(output.data()),
               static_cast<std::streamsize>(output.size()));
    if (!file) {
        error = "archive write failed";
        return false;
    }
    return true;
}

}  // namespace ariel::archive
