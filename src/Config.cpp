#include "Config.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

AppConfig g_Config;

// Save() is called from the UI thread on nearly every settings edit and from
// the MTA worker thread when a quick action changes its persisted state. This
// only serialises the file I/O so two threads can never interleave writes into
// a half-valid config.json - it does not make the individual fields race-free.
static std::mutex g_configFileMutex;

// Last text actually written. Serialising ~50 keys is cheap, opening and
// truncating a file is not, so an identical payload is dropped before it
// reaches the disk. Guarded by g_configFileMutex.
static std::string g_lastWritten;

// GetTickCount64() of the first SaveDeferred() since the last write, 0 when
// nothing is pending. Set by SaveDeferred(), consumed by FlushPendingSave() on
// the message loop.
static std::atomic<ULONGLONG> g_savePendingSince{0};

// A slider reports "changed" on every frame it is dragged, and flushing on
// every pass of the render loop still meant one full config.json rewrite (side
// file, backup copy, write-through rename) per frame. Edits now coalesce for
// this long: a drag reaches the disk a few times a second, and the last value
// lands within this window of the user letting go.
static const ULONGLONG kSaveCoalesceMs = 400;

// Set by ParseInto when it had to move the browsers out of mediaApps, so the
// split reaches config.json without waiting for the user's next settings edit.
static bool g_migratedSecondaryApps = false;
static bool g_migratedHdrHotkeys = false;

// Non-empty when Load() could not use the on-disk file. The settings menu
// surfaces it, so a config that got reset can never again present itself as
// "the app is simply broken". Written on the main thread during Load(), before
// any other thread exists; read afterwards by the render thread.
static std::string g_loadWarning;

// The tracked-app whitelist has exactly one definition. It used to have two -
// a populated list on the first-run path and an empty vector as the parse-path
// fallback - so any config.json missing this one key silently produced an empty
// whitelist. An empty whitelist disables every media feature (volume AND
// play/pause) while the hook still swallows the keys, which is indistinguishable
// from a dead app.
static const std::vector<std::string>& DefaultMediaApps() {
    static const std::vector<std::string> kApps = {
        "spotify.exe", "qobuz.exe", "tidal.exe", "applemusicwin.exe",
        "music.ui.exe", "foobar2000.exe", "aimp.exe", "wmplayer.exe",
        "vlc.exe", "mpc-hc64.exe", "mpc-be64.exe", "mpc-hc.exe", "mpc-be.exe"
    };
    return kApps;
}

// Browsers used to sit in the list above, which is why one Mute press could
// pause a YouTube tab instead of Spotify: both the volume roller and the
// play/pause path act on every whitelisted app, and TogglePlayPause even
// *prefers* whichever matching session is currently playing. They live in their
// own group now and are only reachable while AppConfig::secondaryKey is held.
static const std::vector<std::string>& DefaultSecondaryApps() {
    static const std::vector<std::string> kApps = {
        "msedge.exe", "chrome.exe", "firefox.exe", "brave.exe",
        "opera.exe", "opera_gx.exe", "vivaldi.exe", "yandex.exe",
        "arc.exe", "zen.exe", "msedgewebview2.exe"
    };
    return kApps;
}

// One-time migration for a config written before the split. Only the names this
// app itself ever seeded are moved out of mediaApps; anything the user added by
// hand stays exactly where they put it.
static bool WasSeededAsMediaApp(const std::string& name) {
    for (const auto& app : DefaultSecondaryApps()) {
        if (_stricmp(name.c_str(), app.c_str()) == 0) return true;
    }
    return false;
}

static std::string GetConfigPath() {
    char szPath[MAX_PATH];
    GetModuleFileNameA(NULL, szPath, MAX_PATH);
    return (fs::path(szPath).parent_path() / "config.json").string();
}

static std::string GetBackupPath() { return GetConfigPath() + ".bak"; }

// Moves an unreadable config aside instead of leaving it to be silently
// overwritten by the next Save(). Best effort: if it cannot be renamed the
// caller still carries on with recovered/default settings.
static void QuarantineFile(const std::string& path) {
    char stamp[32];
    _snprintf_s(stamp, sizeof(stamp), _TRUNCATE, ".corrupt-%lld", (long long)std::time(nullptr));
    MoveFileExA(path.c_str(), (path + stamp).c_str(), MOVEFILE_REPLACE_EXISTING);
}

// Parses into a caller-owned config. Nothing reaches the live g_Config until
// the whole document has parsed, so a document that throws part-way through can
// no longer leave half the settings applied and half defaulted.
static HdrHotkey ParseHdrHotkey(const json& j, const char* name, HdrHotkey fallback) {
    if (!j.contains(name)) {
        g_migratedHdrHotkeys = true;
        return fallback;
    }
    const auto& value = j.at(name);
    if (!value.is_object()) return fallback;
    try {
        HdrHotkey binding{value.value("key", fallback.key), value.value("modifiers", fallback.modifiers)};
        if (binding.key == 0 && (binding.modifiers & ~31u) == 0) return binding;
        return binding.Enabled() ? binding : fallback;
    } catch (const json::exception&) {
        return fallback; // a bad HDR binding must not discard unrelated settings
    }
}

static void ParseInto(const json& j, AppConfig& c) {
    c.modifierKey = j.value("modifierKey", 0);
    c.menuKey = j.value("menuKey", (int)VK_F8);
    c.menuCtrl = j.value("menuCtrl", false);
    c.menuShift = j.value("menuShift", false);
    c.menuAlt = j.value("menuAlt", false);
    c.infoKey = j.value("infoKey", (int)VK_F9);
    c.infoCtrl = j.value("infoCtrl", false);
    c.infoShift = j.value("infoShift", false);
    c.infoAlt = j.value("infoAlt", false);
    c.volStep = j.value("volStep", 0.03f);
    c.volStepLarge = j.value("volStepLarge", 0.10f);
    c.volDebounceThreshold = j.value("volDebounceThreshold", 2);
    c.volDebounceWindowMs = j.value("volDebounceWindowMs", 200);

    // Absent key => the built-in list (a config predating this key, or one
    // written by a failed load). A present array is respected exactly as
    // written, including an empty one the user made deliberately - the menu
    // warns about that case rather than silently overriding the choice.
    if (j.contains("mediaApps") && j["mediaApps"].is_array()) {
        c.mediaApps = j["mediaApps"].get<std::vector<std::string>>();
    } else {
        c.mediaApps = DefaultMediaApps();
    }

    // Absent key => this config predates the audio/browser split, so seed the
    // browser group and lift the browsers back out of mediaApps. Present (even
    // empty) is the user's choice and is left alone.
    if (j.contains("secondaryApps") && j["secondaryApps"].is_array()) {
        c.secondaryApps = j["secondaryApps"].get<std::vector<std::string>>();
    } else {
        c.secondaryApps = DefaultSecondaryApps();
        c.mediaApps.erase(std::remove_if(c.mediaApps.begin(), c.mediaApps.end(),
                                         WasSeededAsMediaApp),
                          c.mediaApps.end());
        g_migratedSecondaryApps = true;
    }
    c.secondaryKey = j.value("secondaryKey", (int)VK_PAUSE);
    c.secondaryHoldMs = j.value("secondaryHoldMs", 1500);

    c.appBindings.clear();
    if (j.contains("appBindings") && j["appBindings"].is_array()) {
        for (const auto& b : j["appBindings"]) {
            AppVolumeBinding binding;
            binding.modifierKey = b.value("modifierKey", 0);
            binding.targetApp = b.value("targetApp", std::string{});
            if (binding.modifierKey != 0 && !binding.targetApp.empty()) {
                c.appBindings.push_back(binding);
            }
        }
    }

    c.osdOffsetX = j.value("osdOffsetX", 50);
    c.osdOffsetY = j.value("osdOffsetY", 100);
    c.fontSizeNormal = j.value("fontSizeNormal", 20);
    c.fontSizeLarge = j.value("fontSizeLarge", 26);
    c.fontSizeSmall = j.value("fontSizeSmall", 16);
    c.fontFamily = j.value("fontFamily", "Segoe UI");

    if (j.contains("barColor") && j["barColor"].is_array() && j["barColor"].size() == 4) {
        for (int i = 0; i < 4; i++) c.barColor[i] = j["barColor"][i];
    }

    c.srKey = j.value("srKey", (int)'Q');
    c.srCtrl = j.value("srCtrl", false);
    c.srShift = j.value("srShift", false);
    c.srAlt = j.value("srAlt", true);
    c.srDeviceMatch = j.value("srDeviceMatch", std::string("K7"));
    c.srHighHz = j.value("srHighHz", 384000);
    c.srLowHz = j.value("srLowHz", 192000);
    c.srAutoRevertMinutes = j.value("srAutoRevertMinutes", 360);
    c.srReminderMinutes = j.value("srReminderMinutes", 30);
    c.srRevertOnExit = j.value("srRevertOnExit", true);
    c.srRefreshSettingsApp = j.value("srRefreshSettingsApp", true);
    c.srActive = j.value("srActive", false);
    c.srSavedHz = j.value("srSavedHz", 0);
    c.srRevertAtUnix = j.value("srRevertAtUnix", (long long)0);
    c.srActiveDeviceId = j.value("srActiveDeviceId", std::string{});

    c.swapKey = j.value("swapKey", (int)'S');
    c.swapCtrl = j.value("swapCtrl", false);
    c.swapShift = j.value("swapShift", false);
    c.swapAlt = j.value("swapAlt", true);
    c.swapMonitorMatch = j.value("swapMonitorMatch", std::string("GSM"));
    c.swapInputPc = j.value("swapInputPc", 0x0F);
    c.swapInputConsole = j.value("swapInputConsole", 0x12);
    c.swapUseAmdSideChannel = j.value("swapUseAmdSideChannel", true);
    c.swapMonitorMatchAmd = j.value("swapMonitorMatchAmd", std::string("ULTRAGEAR"));
    c.swapInputPcAmd = j.value("swapInputPcAmd", 0xD0);
    c.swapInputConsoleAmd = j.value("swapInputConsoleAmd", 0x91);
    c.swapSetPrimary = j.value("swapSetPrimary", true);
    c.swapMoveWindows = j.value("swapMoveWindows", true);
    c.swapPrimaryConsoleMatch = j.value("swapPrimaryConsoleMatch", std::string{});
    c.swapReturnDelayMs = j.value("swapReturnDelayMs", 2500);
    c.swapAudioConsole = j.value("swapAudioConsole", std::string("Pebble"));
    c.swapAudioPc = j.value("swapAudioPc", std::string("K7"));
    c.swapConsoleMode = j.value("swapConsoleMode", false);
    c.swapSavedAudioId = j.value("swapSavedAudioId", std::string{});
    c.hdrEffectPreviousMode = j.value("hdrEffectPreviousMode", 0x2D);
    if (c.hdrEffectPreviousMode < 0 || c.hdrEffectPreviousMode > 255 || c.hdrEffectPreviousMode == 0x27) {
        c.hdrEffectPreviousMode = 0x2D;
    }
    c.hdrEffectMonitorId = j.value("hdrEffectMonitorId", std::string{});
    c.windowsHdrHotkey = ParseHdrHotkey(j, "windowsHdrHotkey", HdrHotkey::WindowsDefault());
    c.monitorHdrHotkey = ParseHdrHotkey(j, "monitorHdrHotkey", HdrHotkey::MonitorDefault());
}

// The in-class initialisers in Config.h are the single source of truth for
// every scalar default; only the app whitelist needs filling in.
void AppConfig::ApplyDefaults() {
    AppConfig defaults;
    defaults.mediaApps = DefaultMediaApps();
    defaults.secondaryApps = DefaultSecondaryApps();
    *this = defaults;
}

void AppConfig::RestoreDefaultMediaApps() {
    mediaApps = DefaultMediaApps();
}

void AppConfig::RestoreDefaultSecondaryApps() {
    secondaryApps = DefaultSecondaryApps();
}

std::string AppConfig::LoadWarning() const { return g_loadWarning; }
void AppConfig::ClearLoadWarning() { g_loadWarning.clear(); }

// Runs once on the main thread before any other thread exists (startup order in
// main.cpp), so it deliberately takes no lock - and it must not, because it can
// call Save(), which does.
//
// Ordering matters: try the live file, then the one-generation backup Save()
// keeps, and only then fall back to defaults. Whatever could not be parsed is
// renamed aside first. The old code instead left a corrupt file in place with
// every field defaulted in memory, and the next Save() - a settings edit, or
// QuickActions persisting swap state from the worker - wrote those defaults
// straight over the user's real config. That is a silent, permanent wipe, and
// it is what emptied mediaApps and disabled the whole app.
void AppConfig::Load() {
    g_loadWarning.clear();

    const std::string sources[2] = { GetConfigPath(), GetBackupPath() };
    bool sawFile = false;

    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string& src = sources[attempt];

        std::ifstream file(src, std::ios::binary);
        if (!file.is_open()) continue;
        sawFile = true;

        AppConfig parsed; // header defaults; ParseInto fills in the rest
        bool ok = false;
        try {
            json j;
            file >> j;
            if (j.is_object()) {
                ParseInto(j, parsed);
                ok = true;
            }
        } catch (...) {
            ok = false;
        }
        file.close();

        if (ok) {
            *this = parsed;
            // A one-time migration only sticks once it is written back; until
            // then every launch would redo it and a removal the user made here
            // would not survive a restart.
            if (g_migratedSecondaryApps || g_migratedHdrHotkeys) SaveDeferred();
            if (attempt == 0) {
                // Refresh the backup the moment the live file has proven it
                // parses. Waiting for the first Save() would leave a freshly
                // started instance with no recovery source at all.
                CopyFileA(src.c_str(), GetBackupPath().c_str(), FALSE);
            }
            if (attempt == 1) {
                g_loadWarning = "config.json could not be read - settings were recovered "
                                "from config.json.bak. The unreadable file was kept as "
                                "config.json.corrupt-*.";
                Save(); // republish the recovered settings as the live config
            }
            return;
        }

        QuarantineFile(src);
    }

    ApplyDefaults();
    if (sawFile) {
        g_loadWarning = "config.json could not be read and no usable backup existed - "
                        "default settings were restored. The unreadable file was kept "
                        "as config.json.corrupt-*.";
    }
    Save();
}

void AppConfig::Save() {
    std::lock_guard<std::mutex> fileLock(g_configFileMutex);
    json j;
    j["modifierKey"] = modifierKey;
    j["menuKey"] = menuKey;
    j["menuCtrl"] = menuCtrl;
    j["menuShift"] = menuShift;
    j["menuAlt"] = menuAlt;
    j["infoKey"] = infoKey;
    j["infoCtrl"] = infoCtrl;
    j["infoShift"] = infoShift;
    j["infoAlt"] = infoAlt;
    j["volStep"] = volStep;
    j["volStepLarge"] = volStepLarge;
    j["volDebounceThreshold"] = volDebounceThreshold;
    j["volDebounceWindowMs"] = volDebounceWindowMs;
    j["mediaApps"] = mediaApps;
    j["secondaryApps"] = secondaryApps;
    j["secondaryKey"] = secondaryKey;
    j["secondaryHoldMs"] = secondaryHoldMs;

    json bindingsArr = json::array();
    for (const auto& b : appBindings) {
        json bj;
        bj["modifierKey"] = b.modifierKey;
        bj["targetApp"] = b.targetApp;
        bindingsArr.push_back(bj);
    }
    j["appBindings"] = bindingsArr;
    j["osdOffsetX"] = osdOffsetX;
    j["osdOffsetY"] = osdOffsetY;
    j["fontSizeNormal"] = fontSizeNormal;
    j["fontSizeLarge"] = fontSizeLarge;
    j["fontSizeSmall"] = fontSizeSmall;
    j["fontFamily"] = fontFamily;
    j["barColor"] = { barColor[0], barColor[1], barColor[2], barColor[3] };

    j["srKey"] = srKey;
    j["srCtrl"] = srCtrl;
    j["srShift"] = srShift;
    j["srAlt"] = srAlt;
    j["srDeviceMatch"] = srDeviceMatch;
    j["srHighHz"] = srHighHz;
    j["srLowHz"] = srLowHz;
    j["srAutoRevertMinutes"] = srAutoRevertMinutes;
    j["srReminderMinutes"] = srReminderMinutes;
    j["srRevertOnExit"] = srRevertOnExit;
    j["srRefreshSettingsApp"] = srRefreshSettingsApp;
    j["srActive"] = srActive;
    j["srSavedHz"] = srSavedHz;
    j["srRevertAtUnix"] = srRevertAtUnix;
    j["srActiveDeviceId"] = srActiveDeviceId;

    j["swapKey"] = swapKey;
    j["swapCtrl"] = swapCtrl;
    j["swapShift"] = swapShift;
    j["swapAlt"] = swapAlt;
    j["swapMonitorMatch"] = swapMonitorMatch;
    j["swapInputPc"] = swapInputPc;
    j["swapInputConsole"] = swapInputConsole;
    j["swapUseAmdSideChannel"] = swapUseAmdSideChannel;
    j["swapMonitorMatchAmd"] = swapMonitorMatchAmd;
    j["swapInputPcAmd"] = swapInputPcAmd;
    j["swapInputConsoleAmd"] = swapInputConsoleAmd;
    j["swapSetPrimary"] = swapSetPrimary;
    j["swapMoveWindows"] = swapMoveWindows;
    j["swapPrimaryConsoleMatch"] = swapPrimaryConsoleMatch;
    j["swapReturnDelayMs"] = swapReturnDelayMs;
    j["swapAudioConsole"] = swapAudioConsole;
    j["swapAudioPc"] = swapAudioPc;
    j["swapConsoleMode"] = swapConsoleMode;
    j["swapSavedAudioId"] = swapSavedAudioId;
    j["hdrEffectPreviousMode"] = hdrEffectPreviousMode;
    j["hdrEffectMonitorId"] = hdrEffectMonitorId;
    j["windowsHdrHotkey"] = {{"key", windowsHdrHotkey.key}, {"modifiers", windowsHdrHotkey.modifiers}};
    j["monitorHdrHotkey"] = {{"key", monitorHdrHotkey.key}, {"modifiers", monitorHdrHotkey.modifiers}};

    // A pending deferred save is satisfied by this write, whoever asked for it.
    g_savePendingSince.store(0, std::memory_order_relaxed);

    std::string text = j.dump(4);
    if (text == g_lastWritten) return;

    const std::string path = GetConfigPath();
    const std::string tmp = path + ".tmp";

    // Write to a side file first. The old code opened config.json itself, which
    // truncates it to zero bytes before a single byte of the new content lands -
    // and this app is built around a crash-relaunch net, force-kills
    // SystemSettings.exe and drives display/DDC drivers, so dying inside that
    // window is a real possibility. A truncated config.json then failed to parse
    // on the next launch, which reset every setting.
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file << text;
        file.flush();
        if (!file) return; // out of space / IO error: config.json is untouched
    }

    // Keep one generation back, so even a well-formed but unwanted write stays
    // recoverable. Best effort - on the very first save there is nothing to copy.
    CopyFileA(path.c_str(), GetBackupPath().c_str(), FALSE);

    // Atomic replace: a reader sees either the whole old file or the whole new
    // one, never a half-written one, and a crash can no longer truncate it.
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return; // config.json still holds the previous good content
    }
    g_lastWritten = std::move(text);
}

void AppConfig::SaveDeferred() {
    // Only the first edit of a burst stamps the time, so a long drag is still
    // written every kSaveCoalesceMs rather than only once it stops.
    ULONGLONG expected = 0;
    g_savePendingSince.compare_exchange_strong(expected, GetTickCount64(), std::memory_order_relaxed);
}

DWORD AppConfig::MsUntilPendingSave() const {
    const ULONGLONG since = g_savePendingSince.load(std::memory_order_relaxed);
    if (since == 0) return INFINITE;
    const ULONGLONG elapsed = GetTickCount64() - since;
    return elapsed >= kSaveCoalesceMs ? 0 : (DWORD)(kSaveCoalesceMs - elapsed);
}

void AppConfig::FlushPendingSave(bool force) {
    const DWORD due = MsUntilPendingSave();
    if (due == INFINITE || (due > 0 && !force)) return;
    Save(); // clears the flag
}
