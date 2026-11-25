#include "crypto.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <iterator>

namespace ariel::crypto {

bool dpapi_unprotect(const std::vector<uint8_t>& input, std::vector<uint8_t>& output) {
    if (input.empty()) return false;
    DATA_BLOB encrypted{static_cast<DWORD>(input.size()), const_cast<BYTE*>(input.data())};
    DATA_BLOB plain{0, nullptr};
    if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr, 0, &plain))
        return false;
    if (plain.cbData > 0 && plain.pbData)
        output.assign(plain.pbData, plain.pbData + plain.cbData);
    else
        output.clear();
    LocalFree(plain.pbData);
    return true;
}

bool aes_gcm_decrypt(std::span<const uint8_t> key,
                     std::span<const uint8_t> nonce,
                     std::span<const uint8_t> ciphertext,
                     std::span<const uint8_t> tag,
                     std::vector<uint8_t>& output) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE key_handle = nullptr;
    bool success = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0)
        return false;
    static constexpr wchar_t chaining_mode_property[] = L"ChainingMode";
    static constexpr wchar_t chaining_mode_gcm[] = BCRYPT_CHAIN_MODE_GCM;
    if (BCryptSetProperty(algorithm, chaining_mode_property,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(chaining_mode_gcm)),
                          sizeof(chaining_mode_gcm), 0) != 0)
        goto cleanup;

    {
        struct key_data_blob_header {
            ULONG magic;
            ULONG version;
            ULONG key_size;
        } header{};
        header.magic = BCRYPT_KEY_DATA_BLOB_MAGIC;
        header.version = BCRYPT_KEY_DATA_BLOB_VERSION1;
        header.key_size = static_cast<ULONG>(key.size());

        std::vector<uint8_t> blob(sizeof(header) + key.size());
        std::copy_n(reinterpret_cast<const uint8_t*>(&header), sizeof(header), blob.begin());
        std::copy(key.begin(), key.end(), blob.begin() + sizeof(header));
        if (BCryptImportKey(algorithm, nullptr, BCRYPT_KEY_DATA_BLOB, &key_handle, nullptr, 0,
                            blob.data(), static_cast<DWORD>(blob.size()), 0) != 0)
            goto cleanup;
    }

    {
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = const_cast<uint8_t*>(nonce.data());
        info.cbNonce = static_cast<DWORD>(nonce.size());
        info.pbTag = const_cast<uint8_t*>(tag.data());
        info.cbTag = static_cast<DWORD>(tag.size());
        info.pbAuthData = nullptr;
        info.cbAAD = 0;

        DWORD plain_length = 0;
        const auto cipher = const_cast<uint8_t*>(ciphertext.data());
        if (BCryptDecrypt(key_handle, cipher, static_cast<DWORD>(ciphertext.size()), &info,
                          nullptr, 0, nullptr, 0, &plain_length, 0) != 0)
            goto cleanup;
        output.resize(plain_length);
        if (BCryptDecrypt(key_handle, cipher, static_cast<DWORD>(ciphertext.size()), &info,
                          nullptr, 0, output.data(), plain_length, &plain_length, 0) != 0) {
            output.clear();
            goto cleanup;
        }
        output.resize(plain_length);
        success = true;
    }

cleanup:
    if (key_handle) BCryptDestroyKey(key_handle);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return success;
}

decrypt_result chromium_decrypt(std::span<const uint8_t> blob,
                                std::span<const uint8_t> master_key) {
    if (blob.size() >= 3 && blob[0] == 'v' && blob[1] == '1' && blob[2] == '0') {
        constexpr size_t prefix = 3;
        constexpr size_t nonce_size = 12;
        constexpr size_t tag_size = 16;
        if (master_key.size() != 32 || blob.size() < prefix + nonce_size + tag_size)
            return {decrypt_status::failed, {}};

        const auto nonce = blob.subspan(prefix, nonce_size);
        const auto ciphertext = blob.subspan(prefix + nonce_size,
                                             blob.size() - prefix - nonce_size - tag_size);
        const auto tag = blob.subspan(blob.size() - tag_size, tag_size);
        std::vector<uint8_t> plain;
        if (!aes_gcm_decrypt(master_key, nonce, ciphertext, tag, plain))
            return {decrypt_status::failed, {}};
        return {decrypt_status::v10,
                plain.empty() ? std::string{}
                              : std::string(reinterpret_cast<const char*>(plain.data()), plain.size())};
    }

    if (blob.size() >= 3 && blob[0] == 'v' && blob[1] == '2' && blob[2] == '0')
        return {decrypt_status::unsupported, {}};

    return {decrypt_status::plain,
            blob.empty() ? std::string{}
                         : std::string(reinterpret_cast<const char*>(blob.data()), blob.size())};
}

}  // namespace ariel::crypto
