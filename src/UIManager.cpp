#include "UIManager.h"
#include "Config.h"
#include "AudioManager.h"
#include "AudioDevices.h"
#include "DisplayControl.h"
#include "InputHook.h"
#include "OverlayWindow.h"
#include "QuickActions.h"
#include "StrUtil.h"
#include "Perf.h"
#include <imgui.h>
#include <vector>
#include <algorithm>
#include <climits>

// How long each popup stays up. The quick-action notice gets longer than the
// volume OSD because it is multi-line and reports success/failure of a whole
// mode switch.
static const ULONGLONG kOsdDurationMs = 2500;
static const ULONGLONG kTrackDurationMs = 3000;
static const ULONGLONG kMuteDurationMs = 2500;
static const ULONGLONG kNoticeDurationMs = 5000;

UIManager& UIManager::Get() {
    static UIManager instance;
    return instance;
}

// Hands work to the MTA worker thread instead of touching COM / DDC-CI from the
// render thread (the low-level keyboard hook lives on this thread and must not
// stall). Mirrors what LowLevelKeyboardProc does for a hotkey press.
static void EnqueueWorkerAction(int type, int arg) {
    auto& hook = InputHook::Get();
    std::lock_guard<std::mutex> lock(hook.eventMutex);
    hook.pendingEvents.push_back({type, false, false, -1, arg});
    hook.eventCv.notify_one();
}

// Both whitelists get the same list box. idSuffix keeps the ImGui ids apart.
static void RenderAppListBox(std::vector<std::string>& apps, const char* idSuffix) {
    if (!ImGui::BeginListBox(idSuffix, ImVec2(-FLT_MIN, 120))) return;
    for (size_t i = 0; i < apps.size(); ++i) {
        ImGui::PushID((int)i);
        if (ImGui::Selectable(apps[i].c_str(), false)) {
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::Selectable("Remove")) {
                apps.erase(apps.begin() + i);
                g_Config.SaveDeferred();
                ImGui::EndPopup();
                ImGui::PopID();
                break;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndListBox();
}

static void ScanFontsInternal(std::map<std::string, std::string>& outFonts, HKEY root) {
    HKEY hKey;
    if (RegOpenKeyExA(root, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        char valueName[MAX_PATH];
        DWORD valueNameSize = MAX_PATH;
        BYTE data[MAX_PATH];
        DWORD dataSize = MAX_PATH;
        DWORD index = 0;
        
        while (RegEnumValueA(hKey, index++, valueName, &valueNameSize, NULL, NULL, data, &dataSize) == ERROR_SUCCESS) {
            std::string name = valueName;
            // Remove "(TrueType)" suffix
            size_t pos = name.find(" (");
            if (pos != std::string::npos) name = name.substr(0, pos);
            
            std::string file = (char*)data;
            outFonts[name] = file;
            
            valueNameSize = MAX_PATH;
            dataSize = MAX_PATH;
        }
        RegCloseKey(hKey);
    }
}

void UIManager::EnsureFontsScanned() {
    if (m_fontsScanned) return;
    m_fontsScanned = true;

    systemFonts.clear();
    ScanFontsInternal(systemFonts, HKEY_LOCAL_MACHINE);
    ScanFontsInternal(systemFonts, HKEY_CURRENT_USER);

    fontNames.clear();
    fontNames.reserve(systemFonts.size());
    for (auto const& [name, path] : systemFonts) {
        fontNames.push_back(name);
    }
    std::sort(fontNames.begin(), fontNames.end());
}

void UIManager::ReleaseFontScan() {
    if (!m_fontsScanned) return;
    m_fontsScanned = false;
    // swap-with-empty: clear() on a map/vector keeps the capacity around.
    std::map<std::string, std::string>().swap(systemFonts);
    std::vector<std::string>().swap(fontNames);
}

void UIManager::Initialize() {
    InputHook::Get().OnVolumeChanged = [this](float vol, const std::string& appName) { ShowOSD(vol, appName); };
    InputHook::Get().OnMenuToggled = [this]() { ToggleMenu(); };
    InputHook::Get().OnTrackInfoRequested = [this](const std::string& track) { ShowTrackInfo(track); };
    InputHook::Get().OnKeyCaptured = [this](int vk, int type) { 
        std::lock_guard<std::mutex> lock(m_uiMutex);
        if (type == 0) {
            g_Config.modifierKey = vk; 
            m_isCapturingModifier = false;
        } else if (type == 1) {
            g_Config.menuKey = vk;
            m_isCapturingMenuKey = false;
        } else if (type == 2) {
            g_Config.infoKey = vk;
            m_isCapturingInfoKey = false;
        } else if (type == 4) {
            g_Config.srKey = vk;
            m_isCapturingSampleRateKey = false;
        } else if (type == 5) {
            g_Config.swapKey = vk;
            m_isCapturingSwapKey = false;
        } else if (type == 6) {
            g_Config.secondaryKey = vk;
            m_isCapturingSecondaryKey = false;
        } else if (type == 7 || type == 8) {
            if (vk != VK_ESCAPE) {
                auto& binding = type == 7 ? g_Config.windowsHdrHotkey : g_Config.monitorHdrHotkey;
                binding.key = vk;
            }
            InputHook::Get().SetHdrHotkeys(g_Config.windowsHdrHotkey, g_Config.monitorHdrHotkey);
        } else if (type == 3) {
            // Per-app binding key captured
            if (!m_pendingBindingApp.empty()) {
                AppVolumeBinding binding;
                binding.modifierKey = vk;
                binding.targetApp = m_pendingBindingApp;
                g_Config.appBindings.push_back(binding);
                m_pendingBindingApp.clear();
            }
            m_isCapturingBindingKey = false;
        }
        // Deferred: this lambda runs *inside* the low-level keyboard hook (on
        // InputHook's hook thread), and blocking it on disk I/O stalls every
        // key press on the system - long enough, and Windows unhooks us.
        g_Config.SaveDeferred();
        OverlayWindow::Get().RequestRedraw();
    };
    InputHook::Get().OnAppMuteToggled = [this](bool muted, const std::string& appName, float vol) {
        ShowMuteOSD(muted, appName, vol);
    };
    QuickActions::Get().OnNotice = [this](const std::string& text, bool isError) {
        ShowNotice(text, isError);
    };
}

bool UIManager::HasActiveUI() const {
    return VisibleMask() != 0;
}

unsigned UIManager::VisibleMask() const {
    const ULONGLONG now = GetTickCount64();
    unsigned mask = 0;
    if (m_menuOpen) mask |= kUiMenu;
    if (now - m_osdShowTime < kOsdDurationMs) mask |= kUiOsd;
    if (now - m_trackShowTime < kTrackDurationMs) mask |= kUiTrack;
    if (now - m_muteOsdShowTime < kMuteDurationMs) mask |= kUiMute;
    if (now - m_noticeShowTime < kNoticeDurationMs) mask |= kUiNotice;
    return mask;
}

DWORD UIManager::MsUntilNextExpiry() const {
    const ULONGLONG now = GetTickCount64();
    ULONGLONG next = ULLONG_MAX;
    auto consider = [&](ULONGLONG shownAt, ULONGLONG duration) {
        const ULONGLONG age = now - shownAt;
        if (age < duration) next = (std::min)(next, duration - age);
    };
    consider(m_osdShowTime, kOsdDurationMs);
    consider(m_trackShowTime, kTrackDurationMs);
    consider(m_muteOsdShowTime, kMuteDurationMs);
    consider(m_noticeShowTime, kNoticeDurationMs);
    return next == ULLONG_MAX ? INFINITE : (DWORD)next;
}

// The render loop sleeps until something happens, so every setter that puts
// something on screen has to wake it. These run on the MTA worker thread (and
// the tray menu on the UI thread); RequestRedraw() only posts a message, and is
// called outside the lock so the render thread is never kept waiting on us.
void UIManager::ShowOSD(float volume, const std::string& appName) {
    PERF_ONLY(LONGLONG zero = 0; Perf::osdShown.compare_exchange_strong(zero, Perf::Now());)
    {
        std::lock_guard<std::mutex> lock(m_uiMutex);
        m_lastVolume = volume;
        m_lastAppName = appName;
        m_osdShowTime = GetTickCount64();
    }
    OverlayWindow::Get().RequestRedraw();
}

void UIManager::ShowTrackInfo(const std::string& trackName) {
    if (trackName.empty()) { PERF_ONLY(Perf::trackStart.store(0);) return; }
    PERF_ONLY(LONGLONG zero = 0; Perf::trackShown.compare_exchange_strong(zero, Perf::Now());)
    {
        std::lock_guard<std::mutex> lock(m_uiMutex);
        m_lastTrackName = trackName;
        m_trackShowTime = GetTickCount64();
    }
    OverlayWindow::Get().RequestRedraw();
}

void UIManager::ToggleMenu() {
    {
        std::lock_guard<std::mutex> lock(m_uiMutex);
        m_menuOpen = !m_menuOpen;
    }
    OverlayWindow::Get().RequestRedraw();
}

void UIManager::SetMenuOpen(bool open) {
    {
        std::lock_guard<std::mutex> lock(m_uiMutex);
        m_menuOpen = open;
    }
    OverlayWindow::Get().RequestRedraw();
}

void UIManager::ShowMuteOSD(bool muted, const std::string& appName, float vol) {
    {
        std::lock_guard<std::mutex> lock(m_uiMutex);
        m_lastMuteState = muted;
        m_lastMuteAppName = appName;
        m_lastMuteVol = vol;
        m_muteOsdShowTime = GetTickCount64();
    }
    OverlayWindow::Get().RequestRedraw();
}

void UIManager::ShowNotice(const std::string& text, bool isError) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_uiMutex);
        m_noticeText = text;
        m_noticeIsError = isError;
        m_noticeShowTime = GetTickCount64();
    }
    OverlayWindow::Get().RequestRedraw();
}

std::string UIManager::GetKeyName(int vkCode) {
    if (vkCode == 0) return "None";
    char name[128];
    UINT scanCode = MapVirtualKey(vkCode, MAPVK_VK_TO_VSC);
    switch (vkCode) {
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            scanCode |= 0x100;
            break;
    }
    if (GetKeyNameTextA(scanCode << 16, name, sizeof(name)) == 0) {
        return "VK " + std::to_string(vkCode);
    }
    return std::string(name);
}

void UIManager::Render() {
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (m_menuOpen) {
        RenderMenu();
        InputHook::Get().SetHdrHotkeys(g_Config.windowsHdrHotkey, g_Config.monitorHdrHotkey);
    }

    const ULONGLONG now = GetTickCount64();
    if (now - m_osdShowTime < kOsdDurationMs) {
        RenderOSD();
    }

    if (now - m_muteOsdShowTime < kMuteDurationMs) {
        RenderMuteOSD();
    }

    if (now - m_noticeShowTime < kNoticeDurationMs) {
        RenderNotice();
    }


    if (now - m_trackShowTime < kTrackDurationMs) {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - g_Config.osdOffsetX, ImGui::GetIO().DisplaySize.y - g_Config.osdOffsetY), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.05f, 0.85f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 12));
        
        if (ImGui::Begin("TrackInfo", nullptr, flags)) {
            if (fontNormal) ImGui::PushFont(fontNormal);
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Now Playing:");
            if (fontNormal) ImGui::PopFont();

            ImGui::Spacing();

            if (fontLarge) ImGui::PushFont(fontLarge);
            ImGui::Text("%s", m_lastTrackName.c_str());
            if (fontLarge) ImGui::PopFont();
        }
        ImGui::End();
        
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
}

void UIManager::RenderOSD() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - g_Config.osdOffsetX, ImGui::GetIO().DisplaySize.y - g_Config.osdOffsetY), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
    
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.05f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 12));
    
    if (ImGui::Begin("OSD", nullptr, flags)) {
        if (fontLarge) ImGui::PushFont(fontLarge);
        
        std::string displayAppName = m_lastAppName;
        size_t dotPos = displayAppName.find_last_of('.');
        if (dotPos != std::string::npos) {
            displayAppName = displayAppName.substr(0, dotPos);
        }
        
        ImGui::Text("%s: %d%%", displayAppName.c_str(), (int)(m_lastVolume * 100.0f + 0.5f));
        if (fontLarge) ImGui::PopFont();

        ImGui::Spacing();

        // Premium Progress Bar
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(g_Config.barColor[0], g_Config.barColor[1], g_Config.barColor[2], g_Config.barColor[3]));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
        ImGui::ProgressBar(m_lastVolume, ImVec2(200, 8), "");
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void UIManager::RenderMuteOSD() {
    // Position slightly above the volume OSD to avoid overlap
    float yOffset = (float)g_Config.osdOffsetY;
    // If the volume OSD is also showing, shift the mute OSD up
    if (GetTickCount64() - m_osdShowTime < kOsdDurationMs) {
        yOffset += 70.0f;
    }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - g_Config.osdOffsetX, ImGui::GetIO().DisplaySize.y - yOffset), ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.05f, 0.85f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(15, 12));

    if (ImGui::Begin("MuteOSD", nullptr, flags)) {
        if (fontLarge) ImGui::PushFont(fontLarge);

        std::string displayAppName = m_lastMuteAppName;
        size_t dotPos = displayAppName.find_last_of('.');
        if (dotPos != std::string::npos) {
            displayAppName = displayAppName.substr(0, dotPos);
        }

        if (m_lastMuteState) {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s: MUTED", displayAppName.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "%s: %d%%", displayAppName.c_str(), (int)(m_lastMuteVol * 100.0f + 0.5f));
        }

        if (fontLarge) ImGui::PopFont();

        if (!m_lastMuteState) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(g_Config.barColor[0], g_Config.barColor[1], g_Config.barColor[2], g_Config.barColor[3]));
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
            ImGui::ProgressBar(m_lastMuteVol, ImVec2(200, 8), "");
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// --- Settings-menu helpers -------------------------------------------------
// ImGui edits char buffers, the config holds std::string. Copy in every frame,
// copy back on change, and report "persist now" only once the user leaves the
// field so a config.json write does not happen per keystroke.
static bool StringField(const char* label, std::string& value) {
    char buffer[256];
    strncpy_s(buffer, sizeof(buffer), value.c_str(), _TRUNCATE);
    if (ImGui::InputText(label, buffer, sizeof(buffer))) value = buffer;
    return ImGui::IsItemDeactivatedAfterEdit();
}

static const int kStandardRates[] = { 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 768000 };

static bool RateCombo(const char* label, int& hz) {
    bool changed = false;
    std::string preview = QuickActions::FormatHz(hz) + " (" + std::to_string(hz) + " Hz)";
    if (ImGui::BeginCombo(label, preview.c_str())) {
        for (int rate : kStandardRates) {
            bool selected = (rate == hz);
            std::string item = QuickActions::FormatHz(rate) + " (" + std::to_string(rate) + " Hz)";
            if (ImGui::Selectable(item.c_str(), selected)) {
                hz = rate;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// MCCS input-source values. Monitors may deviate, hence the "Custom" entry and
// the readout of what the monitor actually reports in the table below.
static bool InputSourceCombo(const char* label, int& value) {
    struct Entry { const char* name; int code; };
    static const Entry kEntries[] = {
        { "VGA-1", 0x01 }, { "DVI-1", 0x03 }, { "DVI-2", 0x04 },
        { "DisplayPort-1", 0x0F }, { "DisplayPort-2", 0x10 },
        { "HDMI-1", 0x11 }, { "HDMI-2", 0x12 },
    };

    char preview[64];
    const char* known = nullptr;
    for (const auto& entry : kEntries) {
        if (entry.code == value) { known = entry.name; break; }
    }
    if (known) sprintf_s(preview, "%s (0x%02X)", known, value);
    else sprintf_s(preview, "Custom (0x%02X)", value);

    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (const auto& entry : kEntries) {
            char item[64];
            sprintf_s(item, "%s (0x%02X)", entry.name, entry.code);
            bool selected = (entry.code == value);
            if (ImGui::Selectable(item, selected)) { value = entry.code; changed = true; }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void UIManager::RenderSampleRateTab() {
    auto& quick = QuickActions::Get();

    ImGui::TextWrapped("Lowers the output device's default sample rate for games that lose sound "
                       "or crackle at very high rates, then puts it back on a timer so it is never "
                       "left lowered by accident.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool changed = false;

    ImGui::Text("Hotkey:");
    bool srCtrl = g_Config.srCtrl, srShift = g_Config.srShift, srAlt = g_Config.srAlt;
    if (ImGui::Checkbox("Ctrl##sr", &srCtrl)) { g_Config.srCtrl = srCtrl; changed = true; } ImGui::SameLine();
    if (ImGui::Checkbox("Shift##sr", &srShift)) { g_Config.srShift = srShift; changed = true; } ImGui::SameLine();
    if (ImGui::Checkbox("Left Alt##sr", &srAlt)) { g_Config.srAlt = srAlt; changed = true; } ImGui::SameLine();
    if (m_isCapturingSampleRateKey) {
        ImGui::Button("Press key...##srkey");
    } else if (ImGui::Button((GetKeyName(g_Config.srKey) + "##srkey").c_str())) {
        m_isCapturingSampleRateKey = true;
        InputHook::Get().BeginKeyCapture(4);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Output device (name contains):");
    if (StringField("##srdevice", g_Config.srDeviceMatch)) changed = true;
    ImGui::TextDisabled("Leave empty to use whatever is the current default output.");

    static std::vector<RenderDeviceInfo> devices;
    if (ImGui::Button("Refresh devices##sr")) {
        devices = AudioDevices::ListRenderDevices();
    }
    if (!devices.empty()) {
        if (ImGui::BeginListBox("##srdevices", ImVec2(-FLT_MIN, 90))) {
            for (size_t i = 0; i < devices.size(); ++i) {
                const auto& device = devices[i];
                char item[320];
                sprintf_s(item, "%s%s  -  %s, %d-bit, %dch",
                          device.isDefault ? "* " : "  ",
                          device.name.c_str(),
                          QuickActions::FormatHz(device.sampleRate).c_str(),
                          device.bitDepth, device.channels);
                ImGui::PushID((int)i);
                if (ImGui::Selectable(item, false)) {
                    g_Config.srDeviceMatch = device.name;
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        ImGui::TextDisabled("Click a device to target it. * = current default.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (RateCombo("Game rate##srlow", g_Config.srLowHz)) changed = true;
    if (RateCombo("Normal rate##srhigh", g_Config.srHighHz)) changed = true;

    ImGui::Spacing();

    // The slider formats keep a %d so ctrl+click typing still works, with the
    // human-readable duration appended.
    std::string revertLabel = g_Config.srAutoRevertMinutes > 0
        ? "%d min  (" + QuickActions::FormatDuration((long long)g_Config.srAutoRevertMinutes * 60) + ")"
        : std::string("off - stays lowered");
    if (ImGui::SliderInt("Auto-restore after", &g_Config.srAutoRevertMinutes, 0, 1440, revertLabel.c_str())) changed = true;

    std::string reminderLabel = g_Config.srReminderMinutes > 0
        ? "%d min"
        : std::string("off");
    if (ImGui::SliderInt("Remind every", &g_Config.srReminderMinutes, 0, 180, reminderLabel.c_str())) changed = true;

    bool revertOnExit = g_Config.srRevertOnExit;
    if (ImGui::Checkbox("Also restore when this app exits", &revertOnExit)) {
        g_Config.srRevertOnExit = revertOnExit;
        changed = true;
    }

    bool refreshSettings = g_Config.srRefreshSettingsApp;
    if (ImGui::Checkbox("Force-close Settings app after a change", &refreshSettings)) {
        g_Config.srRefreshSettingsApp = refreshSettings;
        changed = true;
    }
    ImGui::TextDisabled("Windows' Settings app caches the Format dropdown and can keep\n"
                        "showing the old rate even though the change is real. Closing it\n"
                        "(it relaunches fresh next time) is the only way to make it show\n"
                        "the correct value. Only closes it when the change actually reached\n"
                        "the live audio engine.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (quick.IsSampleRateLowered()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Lowered to %s on \"%s\"",
                           QuickActions::FormatHz(quick.CurrentSampleRate()).c_str(),
                           quick.DeviceLabel().c_str());
        long long remaining = quick.RemainingSeconds();
        if (remaining >= 0) {
            ImGui::Text("Restores automatically in %s", QuickActions::FormatDuration(remaining).c_str());
        } else {
            ImGui::TextDisabled("No auto-restore armed.");
        }
        if (ImGui::Button("Restore now##sr", ImVec2(-1, 30))) EnqueueWorkerAction(5, 1);
    } else {
        ImGui::TextDisabled("Sample rate is not lowered.");
        if (ImGui::Button("Lower now##sr", ImVec2(-1, 30))) EnqueueWorkerAction(5, 2);
    }
    ImGui::TextDisabled("Apps that are already playing may need a restart to pick up a new rate.");

    if (changed) g_Config.SaveDeferred();
}

void UIManager::RenderSwapTab() {
    auto& quick = QuickActions::Get();

    ImGui::TextWrapped("Switches the dual-input monitor between the PC input and the console input "
                       "over DDC/CI, moves the desktop to the other screen and points the default "
                       "audio output at the chosen device.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool changed = false;

    ImGui::Text("Hotkey:");
    bool swapCtrl = g_Config.swapCtrl, swapShift = g_Config.swapShift, swapAlt = g_Config.swapAlt;
    if (ImGui::Checkbox("Ctrl##swap", &swapCtrl)) { g_Config.swapCtrl = swapCtrl; changed = true; } ImGui::SameLine();
    if (ImGui::Checkbox("Shift##swap", &swapShift)) { g_Config.swapShift = swapShift; changed = true; } ImGui::SameLine();
    if (ImGui::Checkbox("Left Alt##swap", &swapAlt)) { g_Config.swapAlt = swapAlt; changed = true; } ImGui::SameLine();
    if (m_isCapturingSwapKey) {
        ImGui::Button("Press key...##swapkey");
    } else if (ImGui::Button((GetKeyName(g_Config.swapKey) + "##swapkey").c_str())) {
        m_isCapturingSwapKey = true;
        InputHook::Get().BeginKeyCapture(5);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Dual-input monitor (id/name contains):");
    if (StringField("##swapmonitor", g_Config.swapMonitorMatch)) changed = true;

    if (ImGui::Button("Rescan monitors##swap")) EnqueueWorkerAction(7, 0);
    ImGui::SameLine();
    ImGui::TextDisabled("(talks to the monitor, takes a moment)");

    std::vector<MonitorInfo> monitors = quick.Monitors();
    int matchedInput = -1;
    if (monitors.empty()) {
        ImGui::TextDisabled(quick.MonitorsScanned() ? "No monitors reported." : "Not scanned yet.");
    } else if (ImGui::BeginTable("##monitors", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX, ImVec2(0, 130))) {
        ImGui::TableSetupColumn("Display", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Monitor", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("##use", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)monitors.size(); ++i) {
            const auto& monitor = monitors[i];
            bool isMatch = DisplayControl::Matches(monitor, g_Config.swapMonitorMatch);
            if (isMatch && monitor.currentInput >= 0 && matchedInput < 0) matchedInput = monitor.currentInput;

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (isMatch) {
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "%s%s",
                                   monitor.deviceName.c_str(), monitor.isPrimary ? " *" : "");
            } else {
                ImGui::Text("%s%s", monitor.deviceName.c_str(), monitor.isPrimary ? " *" : "");
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%s", monitor.description.c_str());
            if (ImGui::IsItemHovered() && !monitor.deviceId.empty()) {
                ImGui::SetTooltip("%s", monitor.deviceId.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%dx%d @%d", monitor.width, monitor.height, monitor.refreshHz);

            ImGui::TableSetColumnIndex(3);
            if (monitor.currentInput >= 0) ImGui::Text("0x%02X", monitor.currentInput);
            else ImGui::TextDisabled("no DDC");

            ImGui::TableSetColumnIndex(4);
            ImGui::PushID(i);
            if (ImGui::SmallButton("Use")) {
                g_Config.swapMonitorMatch = monitor.deviceId.empty() ? monitor.description : monitor.deviceId;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("* = primary. \"Use\" targets that monitor for input switching.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (InputSourceCombo("PC input##swap", g_Config.swapInputPc)) changed = true;
    if (InputSourceCombo("Console input##swap", g_Config.swapInputConsole)) changed = true;

    if (matchedInput >= 0) {
        ImGui::Text("Monitor currently reports 0x%02X.", matchedInput);
        if (ImGui::SmallButton("Save as PC input")) { g_Config.swapInputPc = matchedInput; changed = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton("Save as console input")) { g_Config.swapInputConsole = matchedInput; changed = true; }
        ImGui::TextDisabled("Tip: select an input on the monitor itself, rescan, then save it here.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool useAmd = g_Config.swapUseAmdSideChannel;
    if (ImGui::Checkbox("Fall back to AMD side channel if the write above is ignored", &useAmd)) {
        g_Config.swapUseAmdSideChannel = useAmd;
        changed = true;
    }
    ImGui::TextDisabled("Some LG monitors ACK the standard input-switch write and then\n"
                        "silently ignore it. They expose an undocumented side channel\n"
                        "instead (verified working on an LG UltraGear via AMD's ADL).\n"
                        "This only engages when the standard write's read-back check\n"
                        "reports it did not take - it requires an AMD GPU.");
    if (useAmd) {
        ImGui::Text("Monitor name for AMD ADL (different naming than above):");
        if (StringField("##swapmonitoramd", g_Config.swapMonitorMatchAmd)) changed = true;

        int pcAmd = g_Config.swapInputPcAmd;
        int consoleAmd = g_Config.swapInputConsoleAmd;
        if (ImGui::InputInt("PC side-channel value (hex)##pcamd", &pcAmd, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) {
            g_Config.swapInputPcAmd = pcAmd;
            changed = true;
        }
        if (ImGui::InputInt("Console side-channel value (hex)##consoleamd", &consoleAmd, 0, 0, ImGuiInputTextFlags_CharsHexadecimal)) {
            g_Config.swapInputConsoleAmd = consoleAmd;
            changed = true;
        }
        ImGui::TextDisabled("Defaults (0xD0 / 0x91) are what worked on this monitor.\n"
                            "Other LG models may need different values.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool setPrimary = g_Config.swapSetPrimary;
    if (ImGui::Checkbox("Move the desktop to the other screen", &setPrimary)) {
        g_Config.swapSetPrimary = setPrimary;
        changed = true;
    }
    if (g_Config.swapSetPrimary) {
        bool moveWindows = g_Config.swapMoveWindows;
        if (ImGui::Checkbox("Also drag open windows onto it", &moveWindows)) {
            g_Config.swapMoveWindows = moveWindows;
            changed = true;
        }
        ImGui::TextDisabled("Windows does not relocate existing windows when the primary\n"
                            "display changes; this moves them for you.");

        ImGui::Text("Primary in console mode (empty = first other display):");
        if (StringField("##swapprimary", g_Config.swapPrimaryConsoleMatch)) changed = true;
        if (ImGui::SliderInt("Link-return delay (ms)", &g_Config.swapReturnDelayMs, 0, 8000)) changed = true;
        ImGui::TextDisabled("Time given to the monitor to re-establish the PC link before the\n"
                            "desktop moves back.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Default output in console mode (name contains):");
    if (StringField("##swapaudioconsole", g_Config.swapAudioConsole)) changed = true;
    ImGui::Text("Default output back in PC mode (empty = restore previous):");
    if (StringField("##swapaudiopc", g_Config.swapAudioPc)) changed = true;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (quick.IsConsoleMode()) {
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Mode: CONSOLE");
        if (ImGui::Button("Back to PC##swap", ImVec2(-1, 30))) EnqueueWorkerAction(6, 1);
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "Mode: PC");
        if (ImGui::Button("Switch to console##swap", ImVec2(-1, 30))) EnqueueWorkerAction(6, 2);
    }
    ImGui::TextDisabled("Needs DDC/CI enabled in the monitor's own menu.");

    if (changed) g_Config.SaveDeferred();
}

void UIManager::RenderNotice() {
    // Top-centre, away from the bottom-right OSD stack: these are mode changes
    // and reminders, which are meant to be noticed rather than glanced at.
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 40.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.05f, 0.88f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));

    if (ImGui::Begin("QuickActionNotice", nullptr, flags)) {
        if (fontNormal) ImGui::PushFont(fontNormal);
        ImVec4 color = m_noticeIsError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : ImVec4(0.45f, 0.85f, 1.0f, 1.0f);
        ImGui::TextColored(color, "%s", m_noticeText.c_str());
        if (fontNormal) ImGui::PopFont();
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void UIManager::RenderHdrTab() {
    auto& hook = InputHook::Get();
    auto shortcutName = [this](const HdrHotkey& binding) {
        if (!binding.Enabled()) return std::string("Disabled");
        std::string name;
        if (binding.modifiers & HdrHotkey::Ctrl) name += "Ctrl + ";
        if (binding.modifiers & HdrHotkey::Shift) name += "Shift + ";
        if (binding.modifiers & HdrHotkey::Alt) name += "Left Alt + ";
        if (binding.modifiers & HdrHotkey::Win) name += "Win + ";
        if (binding.modifiers & HdrHotkey::Mute) name += "Mute + ";
        return name + GetKeyName(binding.key);
    };
    auto editor = [&](const char* title, HdrHotkey& binding, int captureType, HdrHotkey defaults) {
        ImGui::PushID(captureType);
        ImGui::TextUnformatted(title);
        ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "%s", shortcutName(binding).c_str());
        bool changed = false;
        const char* labels[] = {"Mute", "Win", "Ctrl", "Shift", "Left Alt"};
        const unsigned flags[] = {HdrHotkey::Mute, HdrHotkey::Win, HdrHotkey::Ctrl, HdrHotkey::Shift, HdrHotkey::Alt};
        for (int i = 0; i < 5; ++i) {
            if (i != 0) ImGui::SameLine();
            bool checked = (binding.modifiers & flags[i]) != 0;
            if (ImGui::Checkbox(labels[i], &checked)) {
                if (checked) binding.modifiers |= flags[i];
                else binding.modifiers &= ~flags[i];
                changed = true;
            }
        }
        const bool capturing = hook.isCapturingKey.load(std::memory_order_acquire) &&
            hook.captureType.load(std::memory_order_relaxed) == captureType;
        if (capturing) {
            ImGui::Button("Press a key (Esc cancels)");
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) hook.isCapturingKey.store(false, std::memory_order_relaxed);
        } else {
            if (ImGui::Button(("Key: " + GetKeyName(binding.key)).c_str())) hook.BeginKeyCapture(captureType);
            ImGui::SameLine();
            if (ImGui::Button("Disable")) { binding.key = 0; changed = true; }
            ImGui::SameLine();
            if (ImGui::Button("Reset default")) { binding = defaults; changed = true; }
        }
        if (changed) {
            hook.SetHdrHotkeys(g_Config.windowsHdrHotkey, g_Config.monitorHdrHotkey);
            g_Config.SaveDeferred();
        }
        ImGui::PopID();
    };

    editor("Monitor HDR Effect (LG 27GS85Q-B)", g_Config.monitorHdrHotkey, 8, HdrHotkey::MonitorDefault());
    ImGui::TextWrapped("Switches the monitor's HDR Effect picture mode and restores your previous preset. "
                       "Requires Windows HDR to be off.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    editor("Windows HDR", g_Config.windowsHdrHotkey, 7, HdrHotkey::WindowsDefault());
    ImGui::TextWrapped("Turns HDR on or off on active HDR-capable displays.");
    if (g_Config.windowsHdrHotkey.Enabled() && g_Config.windowsHdrHotkey == g_Config.monitorHdrHotkey) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.65f, 0.2f, 1.0f));
        ImGui::TextWrapped("These shortcuts are identical. Choose different combinations; "
                           "both HDR shortcuts are inactive until the conflict is resolved.");
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("Changes apply immediately and are saved automatically. Select modifiers above, "
                       "then click Key and press the main key. Hold modifiers and Mute before the main key. "
                       "Use only the selected modifiers; either Win key works.");
    ImGui::TextWrapped("For a modified shortcut that also requires Mute, pressing its main key before Mute "
                       "waits for the chord. Without Mute, the original shortcut runs on release.");
}

void UIManager::RenderMenu() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - 250, ImGui::GetIO().DisplaySize.y / 2 - 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Smart Mixer Settings", &m_menuOpen)) {
        // A config that failed to load used to be invisible: the app came up
        // with defaults, an empty app whitelist swallowed every media key
        // silently, and it just looked dead. Say so instead.
        const std::string loadWarning = g_Config.LoadWarning();
        if (!loadWarning.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
            ImGui::TextWrapped("Config recovery: %s", loadWarning.c_str());
            ImGui::PopStyleColor();
            if (ImGui::SmallButton("Dismiss##loadwarn")) g_Config.ClearLoadWarning();
            ImGui::Separator();
        }

        if (ImGui::BeginTabBar("Tabs")) {
            if (ImGui::BeginTabItem("Controls")) {
                ImGui::Text("Menu Key Combo:");
                
                bool hasCtrl = g_Config.menuCtrl;
                bool hasShift = g_Config.menuShift;
                bool hasAlt = g_Config.menuAlt;
                if (ImGui::Checkbox("Ctrl", &hasCtrl)) { g_Config.menuCtrl = hasCtrl; g_Config.SaveDeferred(); } ImGui::SameLine();
                if (ImGui::Checkbox("Shift", &hasShift)) { g_Config.menuShift = hasShift; g_Config.SaveDeferred(); } ImGui::SameLine();
                if (ImGui::Checkbox("Left Alt", &hasAlt)) { g_Config.menuAlt = hasAlt; g_Config.SaveDeferred(); } ImGui::SameLine();
                
                if (m_isCapturingMenuKey) {
                    ImGui::Button("Press key...##menukey");
                } else {
                    if (ImGui::Button((GetKeyName(g_Config.menuKey) + "##menukey").c_str())) {
                        m_isCapturingMenuKey = true;
                        InputHook::Get().BeginKeyCapture(1);
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Modifier Key for Volume:");
                ImGui::SameLine();
                if (m_isCapturingModifier) {
                    ImGui::Button("Press any key...##modkey");
                } else {
                    if (ImGui::Button((GetKeyName(g_Config.modifierKey) + "##modkey").c_str())) {
                        m_isCapturingModifier = true;
                        InputHook::Get().BeginKeyCapture(0);
                    }
                }
                if (ImGui::Button("Clear Modifier")) {
                    g_Config.modifierKey = 0;
                    g_Config.SaveDeferred();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Track Info Key Combo:");
                bool hasInfoCtrl = g_Config.infoCtrl;
                bool hasInfoShift = g_Config.infoShift;
                bool hasInfoAlt = g_Config.infoAlt;
                if (ImGui::Checkbox("Ctrl##info", &hasInfoCtrl)) { g_Config.infoCtrl = hasInfoCtrl; g_Config.SaveDeferred(); } ImGui::SameLine();
                if (ImGui::Checkbox("Shift##info", &hasInfoShift)) { g_Config.infoShift = hasInfoShift; g_Config.SaveDeferred(); } ImGui::SameLine();
                if (ImGui::Checkbox("Left Alt##info", &hasInfoAlt)) { g_Config.infoAlt = hasInfoAlt; g_Config.SaveDeferred(); } ImGui::SameLine();
                
                if (m_isCapturingInfoKey) {
                    ImGui::Button("Press key...##infokey");
                } else {
                    if (ImGui::Button((GetKeyName(g_Config.infoKey) + "##infokey").c_str())) {
                        m_isCapturingInfoKey = true;
                        InputHook::Get().BeginKeyCapture(2);
                    }
                }
                if (ImGui::Button("Clear Track Key")) {
                    g_Config.infoKey = 0;
                    g_Config.SaveDeferred();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Volume Steps:");
                int normalStep = (int)(g_Config.volStep * 100.0f + 0.5f);
                int largeStep = (int)(g_Config.volStepLarge * 100.0f + 0.5f);
                bool changed = false;
                if (ImGui::SliderInt("Normal Step (%)", &normalStep, 1, 50)) {
                    g_Config.volStep = normalStep / 100.0f;
                    changed = true;
                }
                if (ImGui::SliderInt("Large Step (%) (Shift)", &largeStep, 1, 100)) {
                    g_Config.volStepLarge = largeStep / 100.0f;
                    changed = true;
                }
                if (changed) {
                    g_Config.SaveDeferred();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Accidental Touch Protection (Debounce):");
                changed = false;
                if (ImGui::SliderInt("Min Ticks to Trigger", &g_Config.volDebounceThreshold, 1, 10)) {
                    changed = true;
                }
                if (ImGui::SliderInt("Window (ms)", &g_Config.volDebounceWindowMs, 50, 1000)) {
                    changed = true;
                }
                ImGui::TextDisabled("Threshold: minimum ticks within the window to trigger.\n1 = Instant. For 2+, recommended window is 100-300ms.");
                
                if (changed) {
                    g_Config.SaveDeferred();
                }
                
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("HDR")) {
                RenderHdrTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Applications")) {
                ImGui::Text("Audio Apps (the roller and Mute act on these):");

                // Volume and play/pause are both whitelist-driven, so an empty
                // list means every media key is swallowed and does nothing.
                // That is the exact state a wiped config leaves behind, and
                // without this it gives the user no clue at all.
                if (g_Config.mediaApps.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                    ImGui::TextWrapped("The list is empty, so the volume roller and the Mute key "
                                       "will do nothing. Add an app below, or:");
                    ImGui::PopStyleColor();
                    if (ImGui::Button("Restore default app list")) {
                        g_Config.RestoreDefaultMediaApps();
                        g_Config.SaveDeferred();
                    }
                    ImGui::Spacing();
                }

                RenderAppListBox(g_Config.mediaApps, "##apps");
                ImGui::TextDisabled("Right-click an app to remove it.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Browsers are deliberately kept out of the list above: a bare
                // roller tick or Mute press must never reach a video tab, only
                // a real player. They answer while the hold key is down.
                ImGui::Text("Browser Apps (only while the hold key is down):");
                RenderAppListBox(g_Config.secondaryApps, "##secondaryapps");

                ImGui::Text("Hold Key:");
                ImGui::SameLine();
                if (m_isCapturingSecondaryKey) {
                    ImGui::Button("Press key...##seckey");
                } else {
                    if (ImGui::Button((GetKeyName(g_Config.secondaryKey) + "##seckey").c_str())) {
                        m_isCapturingSecondaryKey = true;
                        InputHook::Get().BeginKeyCapture(6);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Disable##seckey")) {
                    g_Config.secondaryKey = 0;
                    g_Config.SaveDeferred();
                }
                ImGui::SameLine();
                if (ImGui::Button("Restore browser list")) {
                    g_Config.RestoreDefaultSecondaryApps();
                    g_Config.SaveDeferred();
                }

                if (ImGui::SliderInt("Hold grace (ms)", &g_Config.secondaryHoldMs, 0, 5000)) {
                    g_Config.SaveDeferred();
                }
                ImGui::TextWrapped("Hold this key and use the roller or Mute to drive the browser "
                                   "player instead. Pause/Break reports its release the instant it "
                                   "is pressed on most keyboards, so a press also counts as held "
                                   "for the grace window above (0 = require a genuine hold).");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Add Active Audio Session:");
                static std::vector<AudioSessionInfo> activeSessions;
                static int selectedSession = -1;

                if (ImGui::Button("Refresh")) {
                    activeSessions = AudioManager::Get().GetActiveSessions();
                    selectedSession = -1;
                }

                ImGui::SameLine();

                if (ImGui::BeginCombo("##sessions", selectedSession >= 0 && selectedSession < activeSessions.size() ? activeSessions[selectedSession].processName.c_str() : "Select...")) {
                    for (int i = 0; i < activeSessions.size(); ++i) {
                        bool isSelected = (selectedSession == i);
                        if (ImGui::Selectable(activeSessions[i].processName.c_str(), isSelected)) {
                            selectedSession = i;
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                const bool hasSelection = (selectedSession >= 0 && selectedSession < (int)activeSessions.size());
                if (ImGui::Button("Add to audio apps")) {
                    if (hasSelection) {
                        std::string newApp = activeSessions[selectedSession].processName;
                        if (std::find(g_Config.mediaApps.begin(), g_Config.mediaApps.end(), newApp) == g_Config.mediaApps.end()) {
                            g_Config.mediaApps.push_back(newApp);
                            g_Config.SaveDeferred();
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Add to browser apps")) {
                    if (hasSelection) {
                        std::string newApp = activeSessions[selectedSession].processName;
                        if (std::find(g_Config.secondaryApps.begin(), g_Config.secondaryApps.end(), newApp) == g_Config.secondaryApps.end()) {
                            g_Config.secondaryApps.push_back(newApp);
                            g_Config.SaveDeferred();
                        }
                    }
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Per-App Bindings")) {
                ImGui::TextWrapped("Assign a modifier key to control volume/mute for a specific app.\n"
                    "Hold the modifier + Volume wheel to change volume, modifier + Mute to toggle mute.\n"
                    "A binding is checked first, so giving one the same key as the browser hold key "
                    "takes that combo away from the browser list.");
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // List current bindings
                ImGui::Text("Current Bindings:");
                int deleteIdx = -1;
                if (g_Config.appBindings.empty()) {
                    ImGui::TextDisabled("No bindings configured.");
                } else {
                    if (ImGui::BeginTable("##bindings_table", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                        ImGui::TableSetupColumn("Modifier", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                        ImGui::TableSetupColumn("Application", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 60.0f);
                        ImGui::TableHeadersRow();

                        for (int bi = 0; bi < (int)g_Config.appBindings.size(); ++bi) {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%s", GetKeyName(g_Config.appBindings[bi].modifierKey).c_str());
                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%s", g_Config.appBindings[bi].targetApp.c_str());
                            ImGui::TableSetColumnIndex(2);
                            ImGui::PushID(bi);
                            if (ImGui::SmallButton("Remove")) {
                                deleteIdx = bi;
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndTable();
                    }
                }
                if (deleteIdx >= 0) {
                    g_Config.appBindings.erase(g_Config.appBindings.begin() + deleteIdx);
                    g_Config.SaveDeferred();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Add new binding
                ImGui::Text("Add New Binding:");

                // Step 1: Select an app (from active sessions or manual input)
                static std::vector<AudioSessionInfo> bindingSessions;
                static int bindingSelectedSession = -1;
                static char manualAppName[128] = "";

                if (ImGui::Button("Refresh Sessions##binding")) {
                    bindingSessions = AudioManager::Get().GetActiveSessions();
                    bindingSelectedSession = -1;
                }
                ImGui::SameLine();
                if (ImGui::BeginCombo("##binding_sessions", 
                    bindingSelectedSession >= 0 && bindingSelectedSession < (int)bindingSessions.size() 
                        ? bindingSessions[bindingSelectedSession].processName.c_str() 
                        : "Select app...")) {
                    for (int i = 0; i < (int)bindingSessions.size(); ++i) {
                        bool isSelected = (bindingSelectedSession == i);
                        if (ImGui::Selectable(bindingSessions[i].processName.c_str(), isSelected)) {
                            bindingSelectedSession = i;
                            // Copy to manual field
                            strncpy(manualAppName, bindingSessions[i].processName.c_str(), sizeof(manualAppName) - 1);
                            manualAppName[sizeof(manualAppName) - 1] = '\0';
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                ImGui::InputText("Or type app name##binding", manualAppName, sizeof(manualAppName));

                // Step 2: Capture modifier key
                if (m_isCapturingBindingKey) {
                    ImGui::Button("Press modifier key...##bindcap", ImVec2(-1, 30));
                } else {
                    bool canAdd = (manualAppName[0] != '\0');
                    if (!canAdd) ImGui::BeginDisabled();
                    if (ImGui::Button("Capture Modifier Key & Add##bindadd", ImVec2(-1, 30))) {
                        m_pendingBindingApp = manualAppName;
                        m_isCapturingBindingKey = true;
                        InputHook::Get().BeginKeyCapture(3); // 3 = binding key capture
                    }
                    if (!canAdd) ImGui::EndDisabled();
                }
                ImGui::TextDisabled("Select an app, then click the button and press the modifier key.\n"
                    "The binding will be saved automatically.");

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Audio Quality")) {
                RenderSampleRateTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("PC / Console")) {
                RenderSwapTab();
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Customization")) {
                bool changed = false;

                ImGui::Text("OSD Position (Offset from bottom-right):");
                if (ImGui::SliderInt("X Offset", &g_Config.osdOffsetX, 0, 500)) changed = true;
                if (ImGui::SliderInt("Y Offset", &g_Config.osdOffsetY, 0, 500)) changed = true;

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Volume Bar Color:");
                if (ImGui::ColorEdit4("Bar Color", g_Config.barColor)) changed = true;

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Font Style (Search & Preview):");
                EnsureFontsScanned();
                static char fontSearch[64] = "";
                ImGui::InputText("Search##fontsearch", fontSearch, 64);

                if (ImGui::BeginListBox("##fonts", ImVec2(-1, 120))) {
                    // ContainsNoCase compares in place. The previous version
                    // built two lower-case copies per font name per frame,
                    // which over a few hundred installed fonts meant thousands
                    // of allocations for every frame this tab stayed open.
                    for (const auto& name : fontNames) {
                        if (fontSearch[0] != 0 && !ContainsNoCase(name, fontSearch)) continue;
                        
                        bool isSelected = (g_Config.fontFamily == name);
                        if (ImGui::Selectable(name.c_str(), isSelected)) {
                            g_Config.fontFamily = name;
                            changed = true;
                            
                            // Set path for preview (helper in OverlayWindow will resolve it)
                            m_previewFontPath = name; 
                            m_shouldReloadPreview = true;
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndListBox();
                }

                if (fontPreview) {
                    ImGui::Text("Preview:");
                    ImGui::PushFont(fontPreview);
                    ImGui::Text("The quick brown fox jumps over the lazy dog");
                    ImGui::PopFont();
                } else {
                    ImGui::TextDisabled("(Select a font to see preview)");
                }

                ImGui::Spacing();
                
                if (ImGui::SliderInt("Normal Size", &g_Config.fontSizeNormal, 10, 50)) changed = true;
                if (ImGui::SliderInt("Large Size", &g_Config.fontSizeLarge, 10, 50)) changed = true;
                if (ImGui::SliderInt("Small Size", &g_Config.fontSizeSmall, 10, 50)) changed = true;

                if (changed) {
                    g_Config.SaveDeferred();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Restart App to Apply Font Changes", ImVec2(-1, 40))) {
                    g_Config.Save(); // immediate: the process is about to go away
                    char szPath[MAX_PATH];
                    GetModuleFileNameA(NULL, szPath, MAX_PATH);
                    ShellExecuteA(NULL, "open", szPath, NULL, NULL, SW_SHOW);
                    PostQuitMessage(0);
                }

                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}
