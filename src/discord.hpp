#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ariel::discord {

struct hit {
    std::string source;
    std::string token;
};

std::vector<hit> collect(std::vector<std::string>& warnings);
void collect_into(std::vector<hit>& out, std::vector<std::string>& warnings);

std::vector<std::string> find_tokens(std::string_view data);

}  // namespace ariel::discord
