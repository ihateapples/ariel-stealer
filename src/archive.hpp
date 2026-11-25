#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ariel::archive {

struct entry {
    std::string name;
    std::vector<uint8_t> data;
};

bool write_zip(const std::filesystem::path& path,
               const std::vector<entry>& entries,
               const std::string& password,
               std::string& error);

}  // namespace ariel::archive
