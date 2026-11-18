#include "QuickActions.h"
#include "AudioDevices.h"
#include "AmdDdcSideChannel.h"
#include "Config.h"
#include "StrUtil.h"

#include <ctime>
#include <thread>
#include <tlhelp32.h>

using clock_t_ = std::chrono::steady_clock;

static long long NowUnix() {
    return (long long)std::time(nullptr);
}

// SystemSettings.exe (the Settings app host) stays resident in the background
// after being closed, for fast reopen — and it caches the Sound page's Format
// dropdown from whenever it last loaded that page. A rate change made through
// IPolicyConfig::SetDeviceFormat (this app's live-apply path) genuinely reaches
// the audio engine immediately (verified via IAudioClient::GetMixFormat), but
// the cached UI keeps showing the old value regardless — confirmed by
// screenshot: killing this process and reopening Settings is the only way to
// see the correct rate there. There is no public API to invalidate just its
// cache, so the practical fix is to end the whole host process; Windows
// relaunches it fresh, with no data loss, the next time Settings is opened.
static void RefreshSettingsAppCache() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"SystemSettings.exe") == 0) {
                HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (proc) {
                    TerminateProcess(proc, 0);
                    CloseHandle(proc);
                }
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

QuickActions& QuickActions::Get() {
    static QuickActions instance;
    return instance;
}

void QuickActions::HdrAction() {
    std::string report;
    const bool ok = DisplayControl::ToggleHdr(&report);
    Notify(report, !ok);
}

void QuickActions::MonitorHdrEffectAction() {
    // VCP 0x15 = 0x27 (HDR Effect) is verified on the user's 27GS85Q-B.
    // Restrict the vendor-specific command to that model, not every LG panel.
    MonitorInfo target;
    for (const auto& monitor : DisplayControl::ListMonitors(false)) {
        if (!ContainsNoCase(monitor.deviceId, "MONITOR\\GSM5C97\\")) continue;
        if (target.deviceName.empty() || monitor.isPrimary) target = monitor;
    }
    if (target.deviceName.empty()) {
        Notify("LG 27GS85Q-B not found\nHDR Effect requires a connected supported monitor.", true);
        return;
    }
    std::string error;
    bool windowsHdr = false;
    if (!DisplayControl::GetMonitorHdrEnabled(target.deviceName, windowsHdr, &error)) {
        Notify("HDR Effect: " + error, true);
        return;
    }
    if (windowsHdr) {
        Notify("HDR Effect needs an SDR signal\nTurn off Windows HDR first (Settings -> HDR).", true);
        return;
    }
    int current = 0;
    if (!DisplayControl::GetMonitorPictureMode(target.deviceId, current, &error)) {
        Notify("HDR Effect: " + error, true);
        return;
    }
    const bool enable = current != 0x27;
    if (enable) {
        // Save BEFORE changing the monitor so a restart still restores the
        // user's actual previous preset, including changes made in its OSD.
        g_Config.hdrEffectPreviousMode = current;
        g_Config.hdrEffectMonitorId = target.deviceId;
        g_Config.Save();
    }
    const int previous = g_Config.hdrEffectMonitorId == target.deviceId
        ? g_Config.hdrEffectPreviousMode : 0x2D;
    if (!DisplayControl::SetMonitorPictureMode(target.deviceId, enable ? 0x27 : previous, &error)) {
        Notify("HDR Effect: " + error, true);
        return;
    }
    Notify(enable ? "LG: HDR Effect on" : "LG: HDR Effect off\nPrevious picture mode restored.", false);
}

std::string QuickActions::FormatHz(int hz) {
    if (hz <= 0) return "?";
    char buf[32];
    if (hz % 1000 == 0) sprintf_s(buf, "%d kHz", hz / 1000);
    else sprintf_s(buf, "%.1f kHz", hz / 1000.0);
    return buf;
}

std::string QuickActions::FormatDuration(long long seconds) {
    if (seconds < 0) seconds = 0;
    char buf[48];
    if (seconds >= 3600) {
        sprintf_s(buf, "%lldh %lldm", seconds / 3600, (seconds % 3600) / 60);
    } else if (seconds >= 60) {
        sprintf_s(buf, "%lldm", seconds / 60);
    } else {
        sprintf_s(buf, "%llds", seconds);
    }
    return buf;
}

void QuickActions::Notify(const std::string& text, bool isError) {
    if (OnNotice) OnNotice(text, isError);
}

void QuickActions::LoadPersistedState() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_consoleMode = g_Config.swapConsoleMode;
    m_lowered = g_Config.srActive;
    m_savedHz = g_Config.srSavedHz;
    m_currentHz = m_lowered ? g_Config.srLowHz : 0;
    m_revertAtUnix = g_Config.srRevertAtUnix;
    m_deviceId = ToWide(g_Config.srActiveDeviceId);
    m_deviceLabel = g_Config.srDeviceMatch;

    if (!m_lowered) return;

    // A rate left lowered by the previous run is due back either right away (the
    // deadline passed while we were not running) or after whatever is left of it.
    long long remaining = 0;
    if (m_revertAtUnix > 0) {
        remaining = m_revertAtUnix - NowUnix();
        if (remaining < 0) remaining = 0;
        m_hasRevert = true;
        m_revertAt = clock_t_::now() + std::chrono::seconds(remaining);
    }
    if (g_Config.srReminderMinutes > 0) {
        m_hasReminder = true;
        m_reminderAt = clock_t_::now() + std::chrono::minutes(g_Config.srReminderMinutes);
    }
}

bool QuickActions::IsSampleRateLowered() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lowered;
}

int QuickActions::CurrentSampleRate() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentHz;
}

long long QuickActions::RemainingSeconds() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_lowered || m_revertAtUnix <= 0) return -1;
    long long remaining = m_revertAtUnix - NowUnix();
    return remaining < 0 ? 0 : remaining;
}

std::string QuickActions::DeviceLabel() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_deviceLabel;
}

bool QuickActions::IsConsoleMode() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_consoleMode;
}

std::vector<MonitorInfo> QuickActions::Monitors() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_monitors;
}

bool QuickActions::MonitorsScanned() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_monitorsScanned;
}

void QuickActions::ArmTimers(long long revertInSeconds) {
    auto now = clock_t_::now();
    m_hasRevert = revertInSeconds > 0;
    if (m_hasRevert) m_revertAt = now + std::chrono::seconds(revertInSeconds);
    m_hasReminder = g_Config.srReminderMinutes > 0;
    if (m_hasReminder) m_reminderAt = now + std::chrono::minutes(g_Config.srReminderMinutes);
}

void QuickActions::ClearTimers() {
    m_hasRevert = false;
    m_hasReminder = false;
}

bool QuickActions::HasTimer() const {
    return m_hasRevert || m_hasReminder;
}

clock_t_::time_point QuickActions::NextDeadline() const {
    if (m_hasRevert && m_hasReminder) return (std::min)(m_revertAt, m_reminderAt);
    if (m_hasRevert) return m_revertAt;
    return m_reminderAt;
}

// ---------------------------------------------------------------------------
// Sample rate
// ---------------------------------------------------------------------------

bool QuickActions::LowerRate() {
    RenderDeviceInfo device;
    if (!AudioDevices::ResolveDevice(g_Config.srDeviceMatch, device)) {
        Notify(g_Config.srDeviceMatch.empty()
                   ? "No default output device found."
                   : "No output device matching \"" + g_Config.srDeviceMatch + "\".",
               true);
        return false;
    }

    int currentHz = device.sampleRate;
    if (currentHz <= 0) AudioDevices::GetSampleRate(device.id, currentHz);

    const int target = g_Config.srLowHz;

    // The device is already at the game rate — something (a previous run, the
    // control panel) left it there, so the press means "put it back".
    if (currentHz == target) {
        std::string error;
        bool appliedLive = false;
        int normal = g_Config.srHighHz;
        if (!AudioDevices::SetSampleRate(device.id, normal, &error, &appliedLive)) {
            Notify("Could not set " + FormatHz(normal) + ": " + error, true);
            return false;
        }
        if (appliedLive && g_Config.srRefreshSettingsApp) RefreshSettingsAppCache();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lowered = false;
            m_currentHz = normal;
            m_savedHz = 0;
            m_revertAtUnix = 0;
            m_deviceLabel = device.name;
        }
        ClearTimers();
        g_Config.srActive = false;
        g_Config.srSavedHz = 0;
        g_Config.srRevertAtUnix = 0;
        g_Config.srActiveDeviceId.clear();
        g_Config.Save();
        std::string msg = device.name + "\nback to " + FormatHz(normal);
        if (!appliedLive) msg += "\n(stored only - " + error + ")";
        Notify(msg, !appliedLive);
        return true;
    }

    std::string error;
    bool appliedLive = false;
    if (!AudioDevices::SetSampleRate(device.id, target, &error, &appliedLive)) {
        Notify("Could not set " + FormatHz(target) + ": " + error, true);
        return false;
    }
    if (appliedLive && g_Config.srRefreshSettingsApp) RefreshSettingsAppCache();

    const long long revertSeconds = (long long)g_Config.srAutoRevertMinutes * 60;
    const long long revertAtUnix = revertSeconds > 0 ? NowUnix() + revertSeconds : 0;
    const int savedHz = currentHz > 0 ? currentHz : g_Config.srHighHz;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lowered = true;
        m_currentHz = target;
        m_savedHz = savedHz;
        m_revertAtUnix = revertAtUnix;
        m_deviceId = device.id;
        m_deviceLabel = device.name;
    }
    ArmTimers(revertSeconds);

    g_Config.srActive = true;
    g_Config.srSavedHz = savedHz;
    g_Config.srRevertAtUnix = revertAtUnix;
    g_Config.srActiveDeviceId = ToUtf8(device.id);
    g_Config.Save();

    std::string message = device.name + "\n" + FormatHz(savedHz) + " -> " + FormatHz(target);
    if (revertSeconds > 0) message += "\nback automatically in " + FormatDuration(revertSeconds);
    else message += "\nno auto-restore (timer off)";
    if (!appliedLive) message += "\n(stored only - " + error + " - may need a device restart to hear it)";
    Notify(message, !appliedLive);
    return true;
}

bool QuickActions::RestoreRate() {
    std::wstring deviceId;
    int target = 0;
    std::string label;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        deviceId = m_deviceId;
        target = m_savedHz > 0 ? m_savedHz : g_Config.srHighHz;
        label = m_deviceLabel;
    }

    // The endpoint id can be stale (device re-enumerated, USB DAC re-plugged);
    // fall back to resolving it by name again.
    if (deviceId.empty()) {
        RenderDeviceInfo device;
        if (!AudioDevices::ResolveDevice(g_Config.srDeviceMatch, device)) {
            Notify("Cannot restore " + FormatHz(target) + ": device not found.", true);
            return false;
        }
        deviceId = device.id;
        label = device.name;
    }

    std::string error;
    bool appliedLive = false;
    if (!AudioDevices::SetSampleRate(deviceId, target, &error, &appliedLive)) {
        RenderDeviceInfo device;
        bool retried = false;
        if (AudioDevices::ResolveDevice(g_Config.srDeviceMatch, device) && device.id != deviceId) {
            retried = AudioDevices::SetSampleRate(device.id, target, &error, &appliedLive);
            if (retried) label = device.name;
        }
        if (!retried) {
            // Keep the lowered state so the timer (and the tray entry) retry
            // instead of silently forgetting the device is still at game rate.
            ArmTimers(300);
            Notify("Could not restore " + FormatHz(target) + ": " + error + "\nretrying in 5m", true);
            return false;
        }
    }
    if (appliedLive && g_Config.srRefreshSettingsApp) RefreshSettingsAppCache();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lowered = false;
        m_currentHz = target;
        m_savedHz = 0;
        m_revertAtUnix = 0;
        m_deviceId.clear();
        m_deviceLabel = label;
    }
    ClearTimers();

    g_Config.srActive = false;
    g_Config.srSavedHz = 0;
    g_Config.srRevertAtUnix = 0;
    g_Config.srActiveDeviceId.clear();
    g_Config.Save();

    std::string msg = label + "\nrestored to " + FormatHz(target);
    if (!appliedLive) msg += "\n(stored only - " + error + ")";
    Notify(msg, !appliedLive);
    return true;
}

void QuickActions::SampleRateAction(int arg) {
    const bool lowered = IsSampleRateLowered();

    if (arg == 1 || (arg == 0 && lowered)) {
        if (!lowered) {
            Notify("Sample rate is not lowered.", false);
            return;
        }
        RestoreRate();
        return;
    }

    if (arg == 2 && lowered) {
        long long remaining = RemainingSeconds();
        std::string message = "Already at " + FormatHz(CurrentSampleRate());
        if (remaining >= 0) message += "\nback in " + FormatDuration(remaining);
        Notify(message, false);
        return;
    }

    LowerRate();
}

void QuickActions::ProcessTimers() {
    if (!m_hasRevert && !m_hasReminder) return;

    auto now = clock_t_::now();

    if (m_hasRevert && now >= m_revertAt) {
        m_hasRevert = false;
        RestoreRate(); // re-arms itself on failure
        return;
    }

    if (m_hasReminder && now >= m_reminderAt) {
        if (g_Config.srReminderMinutes > 0) {
            m_reminderAt = now + std::chrono::minutes(g_Config.srReminderMinutes);
        } else {
            m_hasReminder = false;
        }
        if (IsSampleRateLowered()) {
            long long remaining = RemainingSeconds();
            std::string message = "Reminder: " + DeviceLabel() + " still at " + FormatHz(CurrentSampleRate());
            message += remaining >= 0 ? "\nback in " + FormatDuration(remaining)
                                      : "\nauto-restore is off";
            Notify(message, false);
        } else {
            m_hasReminder = false;
        }
    }
}

void QuickActions::OnShutdown() {
    if (!g_Config.srRevertOnExit) return;
    if (!IsSampleRateLowered()) return;
    RestoreRate();
}

// ---------------------------------------------------------------------------
// Console / monitor swap
// ---------------------------------------------------------------------------

void QuickActions::RefreshMonitors() {
    std::vector<MonitorInfo> monitors = DisplayControl::ListMonitors(true);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_monitors = monitors;
        m_monitorsScanned = true;
    }

    int matched = 0;
    for (const auto& monitor : monitors) {
        if (DisplayControl::Matches(monitor, g_Config.swapMonitorMatch)) ++matched;
    }

    char buf[160];
    sprintf_s(buf, "Scanned %d monitor(s); %d match \"%s\".",
              (int)monitors.size(), matched, g_Config.swapMonitorMatch.c_str());
    Notify(buf, matched == 0);
}

// Picks the display that should own the desktop while the swap monitor is
// showing the console: the configured one, or failing that the first display
// that is not the swap monitor.
static std::string PickConsolePrimary(const std::vector<MonitorInfo>& monitors) {
    if (!g_Config.swapPrimaryConsoleMatch.empty()) {
        for (const auto& monitor : monitors) {
            if (DisplayControl::Matches(monitor, g_Config.swapPrimaryConsoleMatch)) return monitor.deviceName;
        }
    }
    for (const auto& monitor : monitors) {
        if (!DisplayControl::Matches(monitor, g_Config.swapMonitorMatch)) return monitor.deviceName;
    }
    return {};
}

// The AMD side channel is tried FIRST when enabled, not as a fallback.
// Measured behavior on this monitor: the standard channel's read-back
// verification is unreliable in the PC direction (it has reported both false
// success and true failure across otherwise-identical attempts), and a failed
// standard write immediately followed by a side-channel write appears to
// sometimes make the side-channel write itself fail too - plausibly the two
// commands contending for the same physical I2C bus in quick succession. The
// side channel alone, with no preceding standard attempt, has been reliable
// in every direct test. Standard DDC/CI remains the fallback so a monitor
// that does not need (or was never given) a side-channel value still works.
static bool SwitchMonitorInput(int standardValue, int amdValue, std::string& report, bool& anyError) {
    std::string amdError;
    if (g_Config.swapUseAmdSideChannel &&
        AmdDdcSideChannel::SetInput(g_Config.swapMonitorMatchAmd, amdValue, &amdError)) {
        char buf[80];
        sprintf_s(buf, "Monitor input -> 0x%02X (AMD side channel)\n", amdValue);
        report += buf;
        return true;
    }

    std::string standardError;
    if (DisplayControl::SetMonitorInput(g_Config.swapMonitorMatch, standardValue, &standardError)) {
        char buf[64];
        sprintf_s(buf, "Monitor input -> 0x%02X\n", standardValue);
        report += buf;
        return true;
    }

    report += "Input switch failed: " + standardError;
    if (g_Config.swapUseAmdSideChannel) report += " (AMD side channel also failed: " + amdError + ")";
    report += "\n";
    anyError = true;
    return false;
}

bool QuickActions::EnterConsoleMode() {
    std::vector<MonitorInfo> monitors = DisplayControl::ListMonitors(false);
    std::string report;
    bool anyError = false;

    // Remember where the sound was going so PC mode can put it back.
    std::wstring previousAudio;
    if (!g_Config.swapAudioConsole.empty() && AudioDevices::GetDefaultRenderDeviceId(previousAudio)) {
        g_Config.swapSavedAudioId = ToUtf8(previousAudio);
    }

    // Snapshotted BEFORE SetPrimaryMonitor, while the desktop's coordinate
    // origin still matches what it will be again once EnterPcMode flips
    // primary back — see DisplayControl::SnapshotWindows for why the timing
    // matters (capturing after the flip silently corrupts every restored
    // window's position by the offset between the two monitors' origins).
    std::vector<DisplayControl::MovedWindowState> snapshot;
    if (g_Config.swapMoveWindows) snapshot = DisplayControl::SnapshotWindows();

    // The desktop moves BEFORE the monitor input flips, while the PC still owns
    // the panel — doing it the other way round leaves windows on a screen that
    // is already showing the console.
    if (g_Config.swapSetPrimary && monitors.size() > 1) {
        std::string primary = PickConsolePrimary(monitors);
        std::string error;
        if (primary.empty()) {
            report += "No second display found for primary.\n";
            anyError = true;
        } else if (DisplayControl::SetPrimaryMonitor(primary, &error)) {
            report += "Primary -> " + primary + "\n";
            // Making a display primary does not relocate existing windows, so
            // drag them over explicitly or they stay on the screen the console
            // is about to take over. Recorded so EnterPcMode can put exactly
            // these windows back later, instead of sweeping everything onto
            // whichever monitor is primary at the time.
            if (g_Config.swapMoveWindows) {
                m_movedWindows = DisplayControl::MoveWindowsToMonitor(snapshot, primary);
                report += "Moved " + std::to_string(m_movedWindows.size()) + " window(s)\n";
            }
        } else {
            report += "Primary failed: " + error + "\n";
            anyError = true;
        }
    }

    SwitchMonitorInput(g_Config.swapInputConsole, g_Config.swapInputConsoleAmd, report, anyError);

    if (!g_Config.swapAudioConsole.empty()) {
        RenderDeviceInfo device;
        if (AudioDevices::ResolveDevice(g_Config.swapAudioConsole, device)) {
            // Say so explicitly when the target is already the default, instead
            // of reporting "Output -> X" for what was actually a no-op. Both
            // modes pointing at the same device is a legitimate setup (a DAC
            // shared with the console over its own physical input needs no
            // Windows-side switch) and should not look like a silent failure.
            if (device.isDefault) {
                report += "Output already " + device.name + "\n";
            } else {
                std::string audioError;
                if (AudioDevices::SetDefaultRenderDevice(device.id, &audioError)) {
                    report += "Output -> " + device.name + "\n";
                } else {
                    report += "Output failed: " + audioError + "\n";
                    anyError = true;
                }
            }
        } else {
            report += "No output device matching \"" + g_Config.swapAudioConsole + "\"\n";
            anyError = true;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_consoleMode = true;
    }
    g_Config.swapConsoleMode = true;
    g_Config.Save();

    Notify("Console mode\n" + report, anyError);
    return !anyError;
}

bool QuickActions::EnterPcMode() {
    std::string report;
    bool anyError = false;

    SwitchMonitorInput(g_Config.swapInputPc, g_Config.swapInputPcAmd, report, anyError);

    // Tell the user what is happening before the wait, so the OSD does not
    // appear seconds after the key press.
    Notify("PC mode\n" + report + "waiting for the display link...", anyError);

    if (g_Config.swapSetPrimary) {
        // The monitor drops and re-negotiates its DisplayPort link when the
        // input changes, so Windows needs a moment before it can be made
        // primary again.
        int delay = g_Config.swapReturnDelayMs;
        if (delay < 0) delay = 0;
        if (delay > 15000) delay = 15000;

        std::string primary;
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (attempt == 0) {
                if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }
            for (const auto& monitor : DisplayControl::ListMonitors(false)) {
                if (DisplayControl::Matches(monitor, g_Config.swapMonitorMatch)) {
                    primary = monitor.deviceName;
                    break;
                }
            }
            if (!primary.empty()) break;
        }

        if (primary.empty()) {
            report += "Monitor did not come back - primary unchanged.\n";
            anyError = true;
        } else {
            std::string primaryError;
            if (DisplayControl::SetPrimaryMonitor(primary, &primaryError)) {
                report += "Primary -> " + primary + "\n";
                if (g_Config.swapMoveWindows) {
                    if (!m_movedWindows.empty()) {
                        // Put back exactly the windows EnterConsoleMode moved,
                        // at their exact prior position - not a generic sweep,
                        // which would also drag along anything that was
                        // already on this monitor before the swap started.
                        DisplayControl::RestoreWindows(m_movedWindows);
                        report += "Restored " + std::to_string(m_movedWindows.size()) + " window(s)\n";
                        m_movedWindows.clear();
                    } else {
                        // Nothing recorded (e.g. the app restarted mid-swap) -
                        // fall back to a fresh best-effort sweep so this still
                        // does something reasonable instead of nothing. No
                        // "before" state exists to preserve in this case, so
                        // there is nothing wrong with snapshotting right here.
                        auto freshSnapshot = DisplayControl::SnapshotWindows();
                        auto moved = DisplayControl::MoveWindowsToMonitor(freshSnapshot, primary);
                        report += "Moved " + std::to_string(moved.size()) + " window(s)\n";
                    }
                }
            } else {
                report += "Primary failed: " + primaryError + "\n";
                anyError = true;
            }
        }
    }

    std::string audioTarget = g_Config.swapAudioPc;
    if (!audioTarget.empty()) {
        RenderDeviceInfo device;
        if (AudioDevices::ResolveDevice(audioTarget, device)) {
            if (device.isDefault) {
                report += "Output already " + device.name + "\n";
            } else {
                std::string audioError;
                if (AudioDevices::SetDefaultRenderDevice(device.id, &audioError)) {
                    report += "Output -> " + device.name + "\n";
                } else {
                    report += "Output failed: " + audioError + "\n";
                    anyError = true;
                }
            }
        } else {
            report += "No output device matching \"" + audioTarget + "\"\n";
            anyError = true;
        }
    } else if (!g_Config.swapSavedAudioId.empty()) {
        std::wstring saved = ToWide(g_Config.swapSavedAudioId);
        std::wstring current;
        if (AudioDevices::GetDefaultRenderDeviceId(current) && current == saved) {
            report += "Output already restored\n";
        } else {
            std::string audioError;
            if (AudioDevices::SetDefaultRenderDevice(saved, &audioError)) {
                report += "Output restored\n";
            } else {
                report += "Output restore failed: " + audioError + "\n";
                anyError = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_consoleMode = false;
    }
    g_Config.swapConsoleMode = false;
    g_Config.Save();

    Notify("PC mode\n" + report, anyError);
    return !anyError;
}

void QuickActions::DisplayAction(int arg) {
    // Without a target monitor there is nothing to switch, and the primary-swap
    // step would otherwise move the desktop to an arbitrary display (an empty
    // match never matches, so "first display that is not the swap monitor"
    // would resolve to the first display).
    if (g_Config.swapMonitorMatch.empty()) {
        Notify("Pick the dual-input monitor first\n(Settings -> PC / Console).", true);
        return;
    }

    bool toConsole;
    if (arg == 1) toConsole = false;
    else if (arg == 2) toConsole = true;
    else toConsole = !IsConsoleMode();

    if (toConsole) EnterConsoleMode();
    else EnterPcMode();
}
