#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ariel::util {

namespace fs = std::filesystem;

std::wstring environment(const wchar_t* name);
std::string to_utf8(const std::wstring& value);
std::wstring from_utf8(std::string_view value);
std::string narrow_path(const fs::path& path);

bool read_file(const fs::path& path, std::vector<uint8_t>& out);
bool read_text(const fs::path& path, std::string& out);
bool write_text(const fs::path& path, std::string_view data);

std::string lower(std::string_view value);
std::string trim(std::string_view value);
std::string csv_quote(std::string_view value);
std::string printable(std::string_view value);

std::vector<uint8_t> base64_decode(std::string_view value);

std::optional<std::string> json_string(std::string_view json, std::string_view key);
std::optional<std::string_view> json_raw(std::string_view json, std::string_view key);
std::vector<std::string_view> json_objects(std::string_view array);

}  // namespace ariel::util
