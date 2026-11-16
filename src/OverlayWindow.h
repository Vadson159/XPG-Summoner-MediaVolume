#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <atomic>
#include <string>

class OverlayWindow {
public:
    static OverlayWindow& Get();

    bool Create();
    void Destroy();
    void RunLoop();

    void UpdateClickability(bool clickable);

    // Re-fits the overlay to the primary monitor after a resolution change or a
    // primary-display swap (the console-swap hotkey triggers exactly that).
    void OnDisplayChanged();

    // Says "what is on screen has changed" and wakes the render loop. The loop
    // sleeps indefinitely while nothing is on screen and only draws when told
    // (see RunLoop), so any thread that changes what is shown - the MTA worker
    // via the UIManager Show*/Toggle setters, the tray menu, the key-capture
    // callback - has to call this. Safe to call before the window exists: the
    // request is counted, and the loop's first pass picks it up.
    void RequestRedraw();

    // Says "a popup is probably about to be shown" - called by the worker when
    // it starts on an event that can end in one. If the D3D device was released
    // while idle, the render thread rebuilds it right away, in parallel with the
    // worker's audio work (and, at the default debounce threshold of 2, while
    // the worker still waits for the second roller tick), instead of only once
    // ShowOSD has already been called. Cheap to call often: it does nothing
    // while a device exists or a prewarm is already pending.
    void RequestPrewarm();

private:
    OverlayWindow() = default;
    ~OverlayWindow() = default;

    void UpdateTrayTip();

    // Takes the overlay in and out of DWM's composition path. A visible
    // fullscreen layered TOPMOST window is composited over the whole desktop
    // every frame whether or not it has anything in it, and can keep fullscreen
    // apps off the independent-flip path — so it stays hidden while idle.
    void SetOverlayVisible(bool visible);

    // Draws and presents one frame. false = no usable device right now; the
    // caller retries later.
    bool RenderFrame(bool hasContent);

    // Runs once the overlay has stayed hidden for a while (see RunLoop):
    // releases the D3D device with everything on it, compacts the heap and
    // trims the working set, so the hours this app spends idle cost as little
    // memory as possible. The next frame that shows something rebuilds it.
    void IdleCleanup();

    // Rebuilds the font atlas from the font files (plus the settings menu's
    // preview face when previewName is non-empty), uploads it, and then drops
    // every CPU-side copy of it. Requires an initialised DX11 backend.
    bool RebuildFonts(const std::string& previewName);

    // Tears down the D3D device and ImGui's DX11 backend (if there still is
    // one) and brings both back up on a fresh device: after IdleCleanup
    // released it, or after Present reported it lost.
    bool RecreateDevice();

    // The swapchain only exists while there is something on screen: the first
    // frame that needs one creates it at the window's size (or re-fits the one
    // there is), IdleCleanup releases it. Resizing and releasing both unbind
    // and drop every view of the back buffer first, which DXGI requires.
    bool EnsureSwapChain();
    bool CreateSwapChain(UINT width, UINT height);
    bool ResizeSwapChain(UINT width, UINT height);
    void ReleaseSwapChain();

    HWND m_hwnd = nullptr;
    WNDCLASSEXW m_wc = {};
    bool m_isClickable = false;
    bool m_isVisible = false;
    bool m_dx11BackendReady = false;   // ImGui_ImplDX11_Init holds a live device
    // ...and its shaders/font texture exist. Written only by the render
    // thread; atomic so RequestPrewarm (worker) can read it without a message.
    std::atomic<bool> m_deviceObjectsReady{false};
    std::atomic<bool> m_prewarmRequested{false};
    UINT m_bufferW = 0, m_bufferH = 0; // current back-buffer size, 0 = no swapchain
    std::string m_previewFontName;     // preview face currently in the atlas
    std::string m_resolvedFontFamily;  // g_Config.fontFamily that m_resolvedFontPath answers
    std::string m_resolvedFontPath;

    // Render-loop state (RunLoop). Only the render thread touches these;
    // m_redrawRequests is the one field other threads write.
    std::atomic<unsigned> m_redrawRequests{0};
    int m_framesOwed = 0;              // frames still to draw before going quiet
    ULONGLONG m_nextFrameAt = 0;       // GetTickCount64 pacing for those frames
    ULONGLONG m_lastFrameAt = 0;
    ULONGLONG m_idleCleanupAt = 0;     // GetTickCount64 deadline, 0 = none pending
    std::wstring m_trayTip;

    // D3D11
    Microsoft::WRL::ComPtr<ID3D11Device> m_pd3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pd3dDeviceContext;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_pSwapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_mainRenderTargetView;

    bool CreateDeviceD3D();
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();

    static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
};
