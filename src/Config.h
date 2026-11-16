#pragma once
#include <vector>
#include <string>
#include <windows.h>
#include "HdrHotkey.h"

struct AppVolumeBinding {
    int modifierKey = 0;       // VK code of the modifier (Pause, NumLock, etc.)
    std::string targetApp;     // Process name, e.g. "msedge.exe"
};

struct AppConfig {
    int modifierKey = 0; // 0 = no modifier
    int menuKey = VK_F8;
    bool menuCtrl = false;
    bool menuShift = false;
    bool menuAlt = false;
    
    int infoKey = VK_F9;
    bool infoCtrl = false;
    bool infoShift = false;
    bool infoAlt = false;

    float volStep = 0.03f;
    float volStepLarge = 0.10f;

    int volDebounceThreshold = 2;   // Min ticks before triggering (1 = no debounce)
    int volDebounceWindowMs = 200;  // Time window to accumulate ticks (ms)

    // Dedicated audio apps (Spotify, a DAW, a player). The roller and the Mute
    // key act on these and only these when no modifier is held.
    std::vector<std::string> mediaApps;

    // Browser-type players (a YouTube tab, a web player). Deliberately NOT in
    // mediaApps: a bare roller tick or Mute press must never land on a browser,
    // which is what made Mute pause a video instead of Spotify. They answer
    // only while secondaryKey is held - "Pause + Mute", "Pause + roller".
    std::vector<std::string> secondaryApps;

    int secondaryKey = VK_PAUSE;    // 0 = browser group unreachable
    // Pause/Break reports its key-up the instant it is pressed on many
    // keyboards, so GetAsyncKeyState can never see it as held. A press
    // therefore counts as held for this long, refreshed by every tick it
    // routes, so a long roll does not fall back to the audio apps halfway.
    int secondaryHoldMs = 1500;

    // Per-app volume/mute bindings: modifier + target app
    std::vector<AppVolumeBinding> appBindings;

    // Customization
    int osdOffsetX = 50;
    int osdOffsetY = 100;
    float barColor[4] = { 0.4f, 0.6f, 1.0f, 1.0f };
    int fontSizeNormal = 20;
    int fontSizeLarge = 26;
    int fontSizeSmall = 16;
    std::string fontFamily = "Segoe UI";

    // --- Sample-rate quick toggle (default Alt+Q) -------------------------
    // Drops the DAC's "Default Format" rate for games that mis-handle very high
    // rates, then puts it back on a timer so it is never left there.
    int srKey = 'Q';
    bool srCtrl = false;
    bool srShift = false;
    bool srAlt = true;

    std::string srDeviceMatch = "K7";   // substring of the output device name; "" = current default
    int srHighHz = 384000;              // normal / listening rate
    int srLowHz = 192000;               // game rate
    int srAutoRevertMinutes = 360;      // 0 = never revert on its own
    int srReminderMinutes = 30;         // 0 = no periodic reminder
    bool srRevertOnExit = true;
    bool srRefreshSettingsApp = true;   // relaunch SystemSettings.exe after a live change so it stops showing a stale rate

    // Persisted runtime state so a restart (or the crash-relaunch path) still
    // knows the rate is lowered and when it is due back.
    bool srActive = false;
    int srSavedHz = 0;                  // rate captured before lowering
    long long srRevertAtUnix = 0;       // wall-clock deadline, 0 = none
    std::string srActiveDeviceId;       // exact endpoint id that was modified

    // --- Monitor / console swap (default Alt+S) ---------------------------
    int swapKey = 'S';
    bool swapCtrl = false;
    bool swapShift = false;
    bool swapAlt = true;

    std::string swapMonitorMatch = "GSM";   // the dual-input monitor (LG = "GSM" PnP prefix)
    int swapInputPc = 0x0F;                 // VCP 0x60 value for the PC input (DisplayPort-1)
    int swapInputConsole = 0x12;            // VCP 0x60 value for the console input (HDMI-2)

    // Fallback for monitors (some LG models) that silently ignore the standard
    // VCP 0x60 write: an undocumented AMD-ADL-only side channel. Only engaged
    // when the standard write's read-back verification says it did not take.
    bool swapUseAmdSideChannel = true;
    // ADL enumerates this monitor by its EDID-derived friendly name (e.g. "LG
    // ULTRAGEAR"), NOT the PnP id substring swapMonitorMatch uses - the two
    // match strings are for different APIs and are not interchangeable.
    std::string swapMonitorMatchAmd = "ULTRAGEAR";
    int swapInputPcAmd = 0xD0;              // side-channel value for PC/DisplayPort
    int swapInputConsoleAmd = 0x91;         // side-channel value for console/HDMI-2
    bool swapSetPrimary = true;             // move the desktop to the other screen
    bool swapMoveWindows = true;            // also drag open windows onto the new primary
    std::string swapPrimaryConsoleMatch;    // "" = first display that is not the swap monitor
    int swapReturnDelayMs = 2500;           // wait for the DP link to come back before moving primary
    // This machine's wiring: PC -> K7 over USB, PS5 -> K7 over LINE. In console
    // mode the K7 is listening to LINE, so PC audio sent over USB is inaudible -
    // it has to move to the Pebble instead. Defaulting console to "K7" (as this
    // did) regenerates a config that silently drops PC audio on every swap.
    std::string swapAudioConsole = "Pebble"; // default output while in console mode ("" = leave alone)
    std::string swapAudioPc = "K7";          // "" = restore whatever was default before the swap

    // Persisted runtime state.
    bool swapConsoleMode = false;
    std::string swapSavedAudioId;

    // HDR Effect's previous monitor preset, persisted across application restarts.
    int hdrEffectPreviousMode = 0x2D; // Gamer 1 when no prior preset was recorded
    std::string hdrEffectMonitorId;

    HdrHotkey windowsHdrHotkey = HdrHotkey::WindowsDefault();
    HdrHotkey monitorHdrHotkey = HdrHotkey::MonitorDefault();

    void Load();
    void Save();

    // Resets every field to the in-class defaults above plus the built-in
    // media-app whitelist. Used by Load()'s last-resort path and by the
    // settings menu's recovery buttons.
    void ApplyDefaults();
    void RestoreDefaultMediaApps();
    void RestoreDefaultSecondaryApps();

    // Empty unless Load() had to quarantine an unreadable config.json and fall
    // back to the backup or to defaults. The settings menu shows it so a reset
    // config is never again mistaken for a broken app.
    std::string LoadWarning() const;
    void ClearLoadWarning();

    // Coalesced save for the settings UI and the key-capture callback.
    //
    // Every widget in the menu reports "changed" for as long as it is being
    // dragged, so wiring them straight to Save() rewrote the whole config.json
    // once per rendered frame. The key-capture callback is worse: it runs
    // *inside* the low-level keyboard hook, where blocking on disk I/O risks
    // Windows silently unhooking us past LowLevelHooksTimeout.
    //
    // SaveDeferred() only stamps the time of the first unsaved edit; the
    // overlay's message loop calls FlushPendingSave() and that is where the
    // write happens, once the edits have had kSaveCoalesceMs (Config.cpp) to
    // pile up. MsUntilPendingSave() is how long the loop may sleep before that:
    // INFINITE when nothing is pending, 0 when the write is due. force = write
    // a pending edit now regardless (teardown).
    void SaveDeferred();
    DWORD MsUntilPendingSave() const;
    void FlushPendingSave(bool force = false);
};

extern AppConfig g_Config;
