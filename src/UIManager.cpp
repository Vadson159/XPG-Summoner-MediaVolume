#include "UIManager.h"
#include "Config.h"
#include "AudioManager.h"
#include "InputHook.h"
#include <imgui.h>
#include <vector>
#include <algorithm>

UIManager& UIManager::Get() {
    static UIManager instance;
    return instance;
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

void UIManager::Initialize() {
    // Scan Fonts
    systemFonts.clear();
    ScanFontsInternal(systemFonts, HKEY_LOCAL_MACHINE);
    ScanFontsInternal(systemFonts, HKEY_CURRENT_USER);
    
    fontNames.clear();
    for (auto const& [name, path] : systemFonts) {
        fontNames.push_back(name);
    }
    std::sort(fontNames.begin(), fontNames.end());

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
        }
        g_Config.Save();
    };
}

bool UIManager::HasActiveUI() const {
    return m_menuOpen || ((GetTickCount64() - m_osdShowTime) < 2500) || ((GetTickCount64() - m_trackShowTime) < 3000);
}

void UIManager::ShowOSD(float volume, const std::string& appName) {
    std::lock_guard<std::mutex> lock(m_uiMutex);
    m_lastVolume = volume;
    m_lastAppName = appName;
    m_osdShowTime = GetTickCount64();
}

void UIManager::ShowTrackInfo(const std::string& trackName) {
    if (trackName.empty()) return;
    std::lock_guard<std::mutex> lock(m_uiMutex);
    m_lastTrackName = trackName;
    m_trackShowTime = GetTickCount64();
}

void UIManager::ToggleMenu() {
    std::lock_guard<std::mutex> lock(m_uiMutex);
    m_menuOpen = !m_menuOpen;
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
    }

    if (GetTickCount64() - m_osdShowTime < 2500) {
        RenderOSD();
    }
    
    if (GetTickCount64() - m_trackShowTime < 3000) {
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

void UIManager::RenderMenu() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x / 2 - 250, ImGui::GetIO().DisplaySize.y / 2 - 200), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(500, 400), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Smart Mixer Settings", &m_menuOpen)) {
        if (ImGui::BeginTabBar("Tabs")) {
            if (ImGui::BeginTabItem("Controls")) {
                ImGui::Text("Menu Key Combo:");
                
                bool hasCtrl = g_Config.menuCtrl;
                bool hasShift = g_Config.menuShift;
                bool hasAlt = g_Config.menuAlt;
                if (ImGui::Checkbox("Ctrl", &hasCtrl)) { g_Config.menuCtrl = hasCtrl; g_Config.Save(); } ImGui::SameLine();
                if (ImGui::Checkbox("Shift", &hasShift)) { g_Config.menuShift = hasShift; g_Config.Save(); } ImGui::SameLine();
                if (ImGui::Checkbox("Alt", &hasAlt)) { g_Config.menuAlt = hasAlt; g_Config.Save(); } ImGui::SameLine();
                
                if (m_isCapturingMenuKey) {
                    ImGui::Button("Press key...##menukey");
                } else {
                    if (ImGui::Button((GetKeyName(g_Config.menuKey) + "##menukey").c_str())) {
                        m_isCapturingMenuKey = true;
                        InputHook::Get().isCapturingKey = true;
                        InputHook::Get().captureType = 1;
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
                        InputHook::Get().isCapturingKey = true;
                        InputHook::Get().captureType = 0;
                    }
                }
                if (ImGui::Button("Clear Modifier")) {
                    g_Config.modifierKey = 0;
                    g_Config.Save();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Track Info Key Combo:");
                bool hasInfoCtrl = g_Config.infoCtrl;
                bool hasInfoShift = g_Config.infoShift;
                bool hasInfoAlt = g_Config.infoAlt;
                if (ImGui::Checkbox("Ctrl##info", &hasInfoCtrl)) { g_Config.infoCtrl = hasInfoCtrl; g_Config.Save(); } ImGui::SameLine();
                if (ImGui::Checkbox("Shift##info", &hasInfoShift)) { g_Config.infoShift = hasInfoShift; g_Config.Save(); } ImGui::SameLine();
                if (ImGui::Checkbox("Alt##info", &hasInfoAlt)) { g_Config.infoAlt = hasInfoAlt; g_Config.Save(); } ImGui::SameLine();
                
                if (m_isCapturingInfoKey) {
                    ImGui::Button("Press key...##infokey");
                } else {
                    if (ImGui::Button((GetKeyName(g_Config.infoKey) + "##infokey").c_str())) {
                        m_isCapturingInfoKey = true;
                        InputHook::Get().isCapturingKey = true;
                        InputHook::Get().captureType = 2;
                    }
                }
                if (ImGui::Button("Clear Track Key")) {
                    g_Config.infoKey = 0;
                    g_Config.Save();
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
                    g_Config.Save();
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
                    g_Config.Save();
                }
                
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Applications")) {
                ImGui::Text("Tracked Applications:");
                
                if (ImGui::BeginListBox("##apps", ImVec2(-FLT_MIN, 150))) {
                    for (size_t i = 0; i < g_Config.mediaApps.size(); ++i) {
                        ImGui::PushID((int)i);
                        if (ImGui::Selectable(g_Config.mediaApps[i].c_str(), false)) {
                        }
                        if (ImGui::BeginPopupContextItem()) {
                            if (ImGui::Selectable("Remove")) {
                                g_Config.mediaApps.erase(g_Config.mediaApps.begin() + i);
                                g_Config.Save();
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
                ImGui::TextDisabled("Right-click an app to remove it.");

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

                if (ImGui::Button("Add Selected")) {
                    if (selectedSession >= 0 && selectedSession < activeSessions.size()) {
                        std::string newApp = activeSessions[selectedSession].processName;
                        if (std::find(g_Config.mediaApps.begin(), g_Config.mediaApps.end(), newApp) == g_Config.mediaApps.end()) {
                            g_Config.mediaApps.push_back(newApp);
                            g_Config.Save();
                        }
                    }
                }

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
                static char fontSearch[64] = "";
                ImGui::InputText("Search##fontsearch", fontSearch, 64);
                
                if (ImGui::BeginListBox("##fonts", ImVec2(-1, 120))) {
                    for (const auto& name : fontNames) {
                        // Case insensitive search
                        std::string lowerName = name;
                        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                        std::string lowerSearch = fontSearch;
                        std::transform(lowerSearch.begin(), lowerSearch.end(), lowerSearch.begin(), ::tolower);

                        if (fontSearch[0] != '\0' && lowerName.find(lowerSearch) == std::string::npos) continue;
                        
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
                    g_Config.Save();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::Button("Restart App to Apply Font Changes", ImVec2(-1, 40))) {
                    g_Config.Save();
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
