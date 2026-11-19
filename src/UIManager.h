#pragma once
#include <atomic>
#include <string>
#include <mutex>
#include <windows.h>
#include <imgui.h>
#include <map>
#include <vector>
#include "Autostart.h"

class UIManager {
public:
    static UIManager& Get();

    ImFont* fontNormal = nullptr;
    ImFont* fontLarge = nullptr;
    ImFont* fontPreview = nullptr; // For real-time preview

    std::map<std::string, std::string> systemFonts;
    std::vector<std::string> fontNames;

    void Initialize();
    void Render();

    // Enumerating every installed font costs a registry sweep and a few hundred
    // string allocations that only the font picker in the settings menu ever
    // looks at, so it is done on first use rather than held for the life of the
    // process. Startup needs it exactly once (to resolve the configured font),
    // after which ReleaseFontScan() hands the memory back; opening the
    // Customization tab rebuilds it.
    void EnsureFontsScanned();
    void ReleaseFontScan();
    
    void ShowOSD(float volume, const std::string& appName);
    void ShowTrackInfo(const std::string& trackName);
    void ShowMuteOSD(bool muted, const std::string& appName, float vol);
    // Multi-line status popup used by the quick actions (sample rate / console
    // swap). Called from the worker thread.
    void ShowNotice(const std::string& text, bool isError);
    void ToggleMenu();

    bool IsMenuOpen() const { return m_menuOpen; }
    void SetMenuOpen(bool open);
    // Marks the cached Windows autostart registry state stale so the next
    // Settings menu draw re-reads HKCU\...\Run. Safe to call from any thread.
    void InvalidateAutostartCache();
    bool HasActiveUI() const;

    // What is on screen right now, one bit per element. The render loop only
    // draws when this - or the content behind it, see OverlayWindow::
    // RequestRedraw - changes, so a popup that just sits there for its 2.5 s
    // costs a few frames instead of one every 16 ms. Read without m_uiMutex,
    // like HasActiveUI.
    enum : unsigned { kUiMenu = 1, kUiOsd = 2, kUiTrack = 4, kUiMute = 8, kUiNotice = 16 };
    unsigned VisibleMask() const;
    // Milliseconds until the next timed popup disappears; INFINITE if none is up.
    DWORD MsUntilNextExpiry() const;

private:
    UIManager() = default;
    ~UIManager() = default;

    bool m_menuOpen = false;
    float m_lastVolume = 0.0f;
    std::string m_lastAppName;
    ULONGLONG m_osdShowTime = 0;

    std::string m_lastTrackName;
    ULONGLONG m_trackShowTime = 0;

    // Mute OSD state
    bool m_lastMuteState = false;
    std::string m_lastMuteAppName;
    float m_lastMuteVol = 0.0f;
    ULONGLONG m_muteOsdShowTime = 0;

    // Quick-action notice state
    std::string m_noticeText;
    bool m_noticeIsError = false;
    ULONGLONG m_noticeShowTime = 0;

    bool m_isCapturingModifier = false;
    bool m_isCapturingMenuKey = false;
    bool m_isCapturingInfoKey = false;
    bool m_isCapturingSampleRateKey = false;
    bool m_isCapturingSwapKey = false;
    bool m_isCapturingSecondaryKey = false;

    // Per-app binding capture state
    bool m_isCapturingBindingKey = false;
    std::string m_pendingBindingApp;

    std::mutex m_uiMutex;

    std::string m_previewFontPath;
    bool m_shouldReloadPreview = false;
    bool m_fontsScanned = false;

    // Cached Windows autostart state (read lazily on the UI thread when the
    // Controls tab renders, invalidated on menu open or tray-menu toggle).
    std::atomic<bool> m_autostartDirty{true};
    Autostart::State m_autostartState = Autostart::State::Disabled;
    std::string m_autostartOtherPath;

    void RenderOSD();
    void RenderMuteOSD();
    void RenderNotice();
    void RenderMenu();
    void RenderHdrTab();
    void RenderSampleRateTab();
    void RenderSwapTab();
    std::string GetKeyName(int vkCode);
public:
    bool ShouldReloadPreview() { return m_shouldReloadPreview; }
    void ResetReloadPreview() { m_shouldReloadPreview = false; }
    std::string GetPreviewPath() { return m_previewFontPath; }
};
