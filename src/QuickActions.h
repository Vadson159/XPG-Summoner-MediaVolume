#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "DisplayControl.h"

// The two one-key "modes" on top of the mixer:
//
//   * Sample-rate toggle (default Alt+Q) — drops the DAC's default format to a
//     game-friendly rate and puts it back on a timer, so the high rate is never
//     left off by accident. The armed deadline is persisted in config.json, so a
//     restart (including the crash-relaunch path) picks the timer back up
//     instead of losing it.
//
//   * Console swap (default Alt+S) — flips the dual-input monitor between the
//     PC and the console input over DDC/CI, moves the desktop to the other
//     screen and points the default audio output at the configured DAC.
//
// Every mutating method here talks to COM/WASAPI or the monitor driver and is
// therefore called only from the InputHook MTA worker thread (see the SEH
// wrappers in InputHook.cpp). The const accessors are cheap and mutex-guarded
// so the settings menu can read state from the UI thread.
class QuickActions {
public:
    static QuickActions& Get();

    // Main thread, immediately after g_Config.Load(): restores in-memory state
    // from the persisted fields and re-arms a pending revert.
    void LoadPersistedState();

    // --- Worker thread only ---------------------------------------------
    void SampleRateAction(int arg);  // 0 = toggle, 1 = restore normal, 2 = force lower
    void DisplayAction(int arg);     // 0 = toggle, 1 = force PC, 2 = force console
    void HdrAction();                // default Win + H + Mute: Windows HDR
    void MonitorHdrEffectAction();   // default Mute + H: LG HDR Effect / previous preset
    void RefreshMonitors();
    void ProcessTimers();            // fires the auto-revert / reminder
    void OnShutdown();               // restores the rate if configured to

    bool HasTimer() const;
    std::chrono::steady_clock::time_point NextDeadline() const;

    // --- Any thread ------------------------------------------------------
    bool IsSampleRateLowered() const;
    int CurrentSampleRate() const;      // last rate applied/observed, 0 = unknown
    long long RemainingSeconds() const; // until auto-revert, <0 = no deadline
    std::string DeviceLabel() const;    // friendly name of the device being driven
    bool IsConsoleMode() const;
    std::vector<MonitorInfo> Monitors() const;
    bool MonitorsScanned() const;

    // Fires on the worker thread; UIManager turns it into an on-screen notice.
    std::function<void(const std::string& text, bool isError)> OnNotice;

    static std::string FormatHz(int hz);
    static std::string FormatDuration(long long seconds);

private:
    QuickActions() = default;
    ~QuickActions() = default;

    void Notify(const std::string& text, bool isError);
    bool LowerRate();
    bool RestoreRate();
    void ArmTimers(long long revertInSeconds);
    void ClearTimers();
    bool EnterConsoleMode();
    bool EnterPcMode();

    mutable std::mutex m_mutex;

    bool m_lowered = false;
    int m_currentHz = 0;
    int m_savedHz = 0;
    long long m_revertAtUnix = 0;
    std::wstring m_deviceId;
    std::string m_deviceLabel;

    bool m_consoleMode = false;

    // Windows relocated onto the console-mode monitor, so EnterPcMode can put
    // each one back exactly instead of sweeping everything onto whichever
    // monitor is primary at the time (which would also drag along windows
    // that were already on the second monitor before the swap started).
    // Worker thread only — both EnterConsoleMode and EnterPcMode run there.
    std::vector<DisplayControl::MovedWindowState> m_movedWindows;

    std::vector<MonitorInfo> m_monitors;
    bool m_monitorsScanned = false;

    // Timer state — worker thread only, never read from the UI thread.
    bool m_hasRevert = false;
    std::chrono::steady_clock::time_point m_revertAt;
    bool m_hasReminder = false;
    std::chrono::steady_clock::time_point m_reminderAt;
};
