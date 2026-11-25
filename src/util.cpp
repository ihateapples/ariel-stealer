#include "util.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace ariel::util {

std::wstring environment(const wchar_t* name) {
    const DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (length == 0) return {};
    std::wstring value(length - 1, L'\0');
    GetEnvironmentVariableW(name, value.data(), length);
    return value;
}

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring from_utf8(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

std::string narrow_path(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

bool read_file(const fs::path& path, std::vector<uint8_t>& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const std::streampos end = file.tellg();
    if (end < 0) return false;
    file.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(static_cast<std::streamoff>(end)));
    if (!out.empty())
        file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(file) || out.empty();
}

bool read_text(const fs::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool write_text(const fs::path& path, std::string_view data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(file);
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(first, last - first + 1));
}

std::string csv_quote(std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos)
        return std::string(value);
    std::string result = "\"";
    for (const char c : value) {
        if (c == '"') result += "\"\"";
        else result.push_back(c);
    }
    result.push_back('"');
    return result;
}

std::string printable(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    char buffer[8];
    for (const unsigned char c : value) {
        if (c >= 0x20 && c < 0x7f) {
            result.push_back(static_cast<char>(c));
            continue;
        }
        std::snprintf(buffer, sizeof(buffer), "\\x%02X", c);
        result.append(buffer);
    }
    return result;
}

std::vector<uint8_t> base64_decode(std::string_view value) {
    if (value.empty()) return {};
    DWORD length = 0;
    if (!CryptStringToBinaryA(value.data(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64,
                              nullptr, &length, nullptr, nullptr) || length == 0)
        return {};
    std::vector<uint8_t> result(length);
    if (!CryptStringToBinaryA(value.data(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64,
                              result.data(), &length, nullptr, nullptr))
        return {};
    result.resize(length);
    return result;
}

namespace {

int hex_value(const char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void append_utf8(const unsigned codepoint, std::string& out) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

}  // namespace

std::optional<std::string> json_string(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t search = 0;
    while (true) {
        const size_t pos = json.find(needle, search);
        if (pos == std::string_view::npos) return std::nullopt;
        size_t i = pos + needle.size();
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != ':') {
            search = pos + 1;
            continue;
        }
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != '"') {
            search = pos + 1;
            continue;
        }
        ++i;
        std::string result;
        while (i < json.size()) {
            const char c = json[i++];
            if (c == '"') return result;
            if (c != '\\') {
                result.push_back(c);
                continue;
            }
            if (i >= json.size()) break;
            const char escaped = json[i++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > json.size()) break;
                    unsigned codepoint = 0;
                    for (int k = 0; k < 4; ++k) {
                        const int value = hex_value(json[i + k]);
                        if (value < 0) { codepoint = 0; break; }
                        codepoint = (codepoint << 4) | static_cast<unsigned>(value);
                    }
                    i += 4;
                    append_utf8(codepoint, result);
                    break;
                }
                default: result.push_back(escaped); break;
            }
        }
        search = pos + 1;
    }
}

std::optional<std::string_view> json_raw(std::string_view json, std::string_view key) {
    const std::string needle = "\"" + std::string(key) + "\"";
    size_t search = 0;
    while (true) {
        const size_t pos = json.find(needle, search);
        if (pos == std::string_view::npos) return std::nullopt;
        size_t i = pos + needle.size();
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size() || json[i] != ':') {
            search = pos + 1;
            continue;
        }
        ++i;
        while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
        if (i >= json.size()) return std::nullopt;
        const size_t start = i;
        if (json[i] == '"') {
            ++i;
            bool escaped = false;
            while (i < json.size()) {
                const char c = json[i++];
                if (escaped) { escaped = false; continue; }
                if (c == '\\') { escaped = true; continue; }
                if (c == '"') return json.substr(start, i - start);
            }
            return std::nullopt;
        }
        if (json[i] == '{' || json[i] == '[') {
            const char open = json[i];
            const char close = open == '{' ? '}' : ']';
            int depth = 0;
            bool in_string = false;
            bool escaped = false;
            for (; i < json.size(); ++i) {
                const char c = json[i];
                if (in_string) {
                    if (escaped) escaped = false;
                    else if (c == '\\') escaped = true;
                    else if (c == '"') in_string = false;
                    continue;
                }
                if (c == '"') { in_string = true; continue; }
                if (c == open) ++depth;
                else if (c == close) {
                    --depth;
                    if (depth == 0) return json.substr(start, i + 1 - start);
                }
            }
            return std::nullopt;
        }
        const size_t end = json.find_first_of(",}]", i);
        return json.substr(start, (end == std::string_view::npos ? json.size() : end) - start);
    }
}

std::vector<std::string_view> json_objects(std::string_view array) {
    std::vector<std::string_view> objects;
    bool in_string = false;
    bool escaped = false;
    size_t i = 0;
    while (i < array.size()) {
        if (in_string) {
            const char c = array[i++];
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (array[i] == '"') { in_string = true; ++i; continue; }
        if (array[i] != '{') { ++i; continue; }
        const size_t start = i;
        int depth = 0;
        in_string = false;
        escaped = false;
        for (; i < array.size(); ++i) {
            const char c = array[i];
            if (in_string) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') { in_string = true; continue; }
            if (c == '{') ++depth;
            else if (c == '}') {
                --depth;
                if (depth == 0) {
                    ++i;
                    objects.push_back(array.substr(start, i - start));
                    break;
                }
            }
        }
    }
    return objects;
}

}  // namespace ariel::util
