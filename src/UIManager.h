#pragma once
#include <string>
#include <mutex>
#include <windows.h>
#include <imgui.h>
#include <map>
#include <vector>

class UIManager {
public:
    static UIManager& Get();

    ImFont* fontNormal = nullptr;
    ImFont* fontLarge = nullptr;
    ImFont* fontSmall = nullptr;
    ImFont* fontMono = nullptr;
    ImFont* fontPreview = nullptr; // For real-time preview

    std::map<std::string, std::string> systemFonts;
    std::vector<std::string> fontNames;

    void Initialize();
    void Render();
    
    void ShowOSD(float volume, const std::string& appName);
    void ShowTrackInfo(const std::string& trackName);
    void ToggleMenu();

    bool IsMenuOpen() const { return m_menuOpen; }
    void SetMenuOpen(bool open) { std::lock_guard<std::mutex> lock(m_uiMutex); m_menuOpen = open; }
    bool HasActiveUI() const;

private:
    UIManager() = default;
    ~UIManager() = default;

    bool m_menuOpen = false;
    float m_lastVolume = 0.0f;
    std::string m_lastAppName;
    ULONGLONG m_osdShowTime = 0;

    std::string m_lastTrackName;
    ULONGLONG m_trackShowTime = 0;

    bool m_isCapturingModifier = false;
    bool m_isCapturingMenuKey = false;
    bool m_isCapturingInfoKey = false;

    std::mutex m_uiMutex;

    std::string m_previewFontPath;
    bool m_shouldReloadPreview = false;

    void RenderOSD();
    void RenderMenu();
    std::string GetKeyName(int vkCode);
public:
    bool ShouldReloadPreview() { return m_shouldReloadPreview; }
    void ResetReloadPreview() { m_shouldReloadPreview = false; }
    std::string GetPreviewPath() { return m_previewFontPath; }
};
