#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ariel::crypto {

enum class decrypt_status { plain, v10, unsupported, failed };

struct decrypt_result {
    decrypt_status status;
    std::string value;
};

bool dpapi_unprotect(const std::vector<uint8_t>& input, std::vector<uint8_t>& output);

bool aes_gcm_decrypt(std::span<const uint8_t> key,
                     std::span<const uint8_t> nonce,
                     std::span<const uint8_t> ciphertext,
                     std::span<const uint8_t> tag,
                     std::vector<uint8_t>& output);

decrypt_result chromium_decrypt(std::span<const uint8_t> blob,
                                std::span<const uint8_t> master_key);

}  // namespace ariel::crypto
