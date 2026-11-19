#pragma once
#include <cstddef>
#include <string>

namespace Autostart {

enum class State {
    Disabled,          // No Run value, or suppressed by Task Manager StartupApproved
    EnabledCurrent,    // Run value launches this executable and is approved
    EnabledOtherPath,  // Run value launches a different executable path and is approved
};

// Pure helpers (also exercised directly by AutostartTests).
std::wstring FormatRunCommand(const std::wstring& exePath);
std::wstring ExtractCommandPath(const std::wstring& command);
bool PathsEqual(const std::wstring& a, const std::wstring& b);
bool IsStartupApprovedBytes(const unsigned char* data, size_t size);

// Reads HKCU\Software\Microsoft\Windows\CurrentVersion\Run and
// StartupApproved\Run without modifying anything. When State::EnabledOtherPath
// is returned and outRegisteredPathUtf8 is non-null, receives the other path.
State QueryState(std::string* outRegisteredPathUtf8 = nullptr);

// Enables or disables autostart for the current executable in HKCU\...\Run
// (no elevation required). Enabling also clears any Task Manager disabled
// marker in StartupApproved\Run so Windows actually runs the entry.
bool SetEnabled(bool enable);

} // namespace Autostart
