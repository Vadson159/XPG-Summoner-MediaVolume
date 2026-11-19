#include "Autostart.h"
#include "StrUtil.h"
#include <windows.h>
#include <vector>

namespace Autostart {

namespace {

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKeyPath[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunValueName[] = L"SmartVolumeMixer";

std::wstring GetCurrentExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (len == 0) return {};
        if (len < buf.size() - 1) {
            buf.resize(len);
            return buf;
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
}

std::wstring NormalizePath(std::wstring path) {
    for (wchar_t& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    if (path.size() >= 4 && path[0] == L'\\' && path[1] == L'\\' &&
        path[2] == L'?' && path[3] == L'\\') {
        path.erase(0, 4);
    }
    while (path.size() > 3 && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

bool IsSpace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

bool QueryRunCommand(std::wstring& outCommand) {
    outCommand.clear();
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS st = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &bytes);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        RegCloseKey(hKey);
        return false;
    }

    std::wstring raw((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
    st = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, reinterpret_cast<BYTE*>(raw.data()), &bytes);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) return false;

    raw.resize(bytes / sizeof(wchar_t));
    while (!raw.empty() && raw.back() == L'\0') raw.pop_back();
    if (raw.empty()) return false;

    if (type == REG_EXPAND_SZ) {
        const DWORD need = ExpandEnvironmentStringsW(raw.c_str(), nullptr, 0);
        if (need > 1) {
            std::wstring expanded(need, L'\0');
            const DWORD written = ExpandEnvironmentStringsW(raw.c_str(), expanded.data(), need);
            if (written > 0 && written <= need) {
                expanded.resize(written);
                while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
                outCommand = std::move(expanded);
                return !outCommand.empty();
            }
        }
    }

    outCommand = std::move(raw);
    return !outCommand.empty();
}

bool IsStartupApproved() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKeyPath, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) {
        return true; // absent key means not disabled
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS st = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &bytes);
    if (st != ERROR_SUCCESS || type != REG_BINARY) {
        RegCloseKey(hKey);
        return true;
    }

    std::vector<unsigned char> data(bytes);
    if (bytes > 0) {
        st = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, data.data(), &bytes);
    }
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS) return true;

    return IsStartupApprovedBytes(data.data(), bytes);
}

void ClearStartupApprovedMarker() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedKeyPath, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, kRunValueName);
        RegCloseKey(hKey);
    }
}

} // namespace

std::wstring FormatRunCommand(const std::wstring& exePath) {
    if (exePath.empty()) return {};
    return L"\"" + exePath + L"\"";
}

std::wstring ExtractCommandPath(const std::wstring& command) {
    size_t start = 0;
    while (start < command.size() && IsSpace(command[start])) ++start;
    size_t end = command.size();
    while (end > start && IsSpace(command[end - 1])) --end;
    if (start >= end) return {};

    const std::wstring trimmed = command.substr(start, end - start);
    if (trimmed.front() == L'"') {
        const size_t close = trimmed.find(L'"', 1);
        return close == std::wstring::npos ? trimmed.substr(1) : trimmed.substr(1, close - 1);
    }

    // Unquoted command line: if ".exe" appears before whitespace, strip trailing args.
    for (size_t i = 0; i + 4 <= trimmed.size(); ++i) {
        const wchar_t c0 = trimmed[i];
        const wchar_t c1 = trimmed[i + 1];
        const wchar_t c2 = trimmed[i + 2];
        const wchar_t c3 = trimmed[i + 3];
        if (c0 == L'.' &&
            (c1 == L'e' || c1 == L'E') &&
            (c2 == L'x' || c2 == L'X') &&
            (c3 == L'e' || c3 == L'E')) {
            const size_t after = i + 4;
            if (after == trimmed.size() || IsSpace(trimmed[after])) {
                return trimmed.substr(0, after);
            }
        }
    }
    return trimmed;
}

bool PathsEqual(const std::wstring& a, const std::wstring& b) {
    const std::wstring na = NormalizePath(a);
    const std::wstring nb = NormalizePath(b);
    if (na.empty() || nb.empty()) return false;
    return CompareStringOrdinal(na.c_str(), (int)na.size(),
                                nb.c_str(), (int)nb.size(), TRUE) == CSTR_EQUAL;
}

bool IsStartupApprovedBytes(const unsigned char* data, size_t size) {
    if (!data || size == 0) return true;
    // Windows StartupApproved uses an even low byte (0x02, 0x06) for enabled
    // entries and an odd low byte (0x01, 0x03) for Task Manager-disabled ones.
    return (data[0] & 1u) == 0u;
}

State QueryState(std::string* outRegisteredPathUtf8) {
    if (outRegisteredPathUtf8) outRegisteredPathUtf8->clear();

    std::wstring command;
    if (!QueryRunCommand(command)) return State::Disabled;
    if (!IsStartupApproved()) return State::Disabled;

    const std::wstring registered = ExtractCommandPath(command);
    if (registered.empty()) return State::Disabled;

    const std::wstring current = GetCurrentExePath();
    if (!current.empty() && PathsEqual(registered, current)) {
        return State::EnabledCurrent;
    }
    if (outRegisteredPathUtf8) {
        *outRegisteredPathUtf8 = ToUtf8(registered);
    }
    return State::EnabledOtherPath;
}

bool SetEnabled(bool enable) {
    if (enable) {
        const std::wstring exe = GetCurrentExePath();
        if (exe.empty()) return false;
        const std::wstring cmd = FormatRunCommand(exe);

        HKEY hKey = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0,
                            KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
            return false;
        }
        const DWORD bytes = (DWORD)((cmd.size() + 1) * sizeof(wchar_t));
        const LSTATUS st = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                                          reinterpret_cast<const BYTE*>(cmd.c_str()), bytes);
        RegCloseKey(hKey);
        if (st != ERROR_SUCCESS) return false;

        ClearStartupApprovedMarker();
        return true;
    }

    HKEY hKey = nullptr;
    const LSTATUS openSt = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &hKey);
    bool ok = true;
    if (openSt == ERROR_SUCCESS) {
        const LSTATUS delSt = RegDeleteValueW(hKey, kRunValueName);
        ok = (delSt == ERROR_SUCCESS || delSt == ERROR_FILE_NOT_FOUND);
        RegCloseKey(hKey);
    } else if (openSt != ERROR_FILE_NOT_FOUND) {
        ok = false;
    }

    ClearStartupApprovedMarker();
    return ok;
}

} // namespace Autostart
