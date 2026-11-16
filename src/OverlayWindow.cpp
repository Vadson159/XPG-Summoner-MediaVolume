#include "OverlayWindow.h"
#include "UIManager.h"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <dxgi.h>
#include "resource.h"
#include "InputHook.h"
#include "Config.h"
#include "QuickActions.h"

#pragma comment(lib, "d3d11.lib")
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

static std::string FindFontPath(const std::string& fontName) {
    // Blacklist symbol-only fonts that cause crashes or invisible text
    std::string lowerName = fontName;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    if (lowerName.find("wingdings") != std::string::npos || 
        lowerName.find("webdings") != std::string::npos || 
        lowerName.find("symbol") != std::string::npos ||
        lowerName.find("extra") != std::string::npos) {
        return "";
    }

    UIManager::Get().EnsureFontsScanned();
    auto it = UIManager::Get().systemFonts.find(fontName);
    if (it != UIManager::Get().systemFonts.end()) {
        const std::string& file = it->second;
        
        // Only allow .ttf and .otf
        std::string lowerFile = file;
        std::transform(lowerFile.begin(), lowerFile.end(), lowerFile.begin(), ::tolower);
        if (lowerFile.find(".ttf") == std::string::npos && lowerFile.find(".otf") == std::string::npos) {
            return "";
        }

        if (file.find('\\') == std::string::npos) {
            // Check Windows Fonts
            std::string path = "C:\\Windows\\Fonts\\" + file;
            if (fs::exists(path)) return path;

            // Check User Fonts
            char localAppData[MAX_PATH];
            if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
                path = std::string(localAppData) + "\\Microsoft\\Windows\\Fonts\\" + file;
                if (fs::exists(path)) return path;
            }
        }
        return file;
    }
    return "";
}

static ImFont* SafeAddFont(ImGuiIO& io, const std::string& fontPath, float size) {
    ImFont* font = nullptr;
    if (!fontPath.empty() && fs::exists(fontPath)) {
        font = io.Fonts->AddFontFromFileTTF(fontPath.c_str(), size, NULL, io.Fonts->GetGlyphRangesCyrillic());
    }
    
    if (!font) {
        // Fallback to Segoe UI
        std::string segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
        if (fs::exists(segoe)) {
            font = io.Fonts->AddFontFromFileTTF(segoe.c_str(), size, NULL, io.Fonts->GetGlyphRangesCyrillic());
        }
    }

    if (!font) {
        font = io.Fonts->AddFontDefault();
    }
    return font;
}


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#define WM_TRAYICON (WM_USER + 1)
#define IDT_TRAY_REFRESH 42
// Posted by RequestRedraw() purely to break the render loop out of its wait.
#define WM_OVERLAY_WAKE (WM_APP + 1)

// Frame pacing while frames are being drawn. The OSD is two lines of text and a
// bar; 60 Hz is already more than it needs, and this is the thread that owns the
// low-level keyboard hook, so keeping its duty cycle low is worth more than a
// higher frame rate. Present() stays non-blocking (no vsync wait) for the same
// reason.
static const ULONGLONG kFrameIntervalMs = 16;

// Frames drawn after anything on screen changes; after that the loop goes quiet
// and the last presented frame simply stays up. One is not enough for ImGui: an
// auto-resizing window is laid out hidden on the frame it appears, and one whose
// text just grew is clipped for a frame before it re-fits. Three covers both
// with one to spare.
static const int kSettleFrames = 3;

// With the settings menu open and nothing happening, it is still redrawn this
// often so time-based text in it (the sample-rate countdown) stays current.
static const ULONGLONG kMenuRefreshMs = 1000;

// How long the overlay has to stay hidden before IdleCleanup runs: long enough
// that a roller session's OSD coming and going does not resize the swapchain
// back and forth every few seconds.
static const ULONGLONG kIdleCleanupDelayMs = 30000;

// How soon to try again when a frame could not be drawn for want of a device.
static const ULONGLONG kDeviceRetryMs = 2000;

OverlayWindow& OverlayWindow::Get() {
    static OverlayWindow instance;
    return instance;
}

bool OverlayWindow::Create() {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    m_wc = { sizeof(m_wc), CS_CLASSDC, WndProc, 0L, 0L, hInst, 
             (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR), 
             nullptr, nullptr, nullptr, L"SmartMixerClass", 
             (HICON)LoadImage(hInst, MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR) };
    RegisterClassExW(&m_wc);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    m_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        m_wc.lpszClassName, L"Smart Volume Mixer Overlay",
        WS_POPUP,
        0, 0, screenW, screenH,
        nullptr, nullptr, m_wc.hInstance, nullptr);

    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    
    if (!CreateDeviceD3D()) {
        CleanupDeviceD3D();
        UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
        return false;
    }

    // Deliberately NOT shown here: the overlay is only made visible when there
    // is something to draw (SetOverlayVisible). A side effect is that it no
    // longer steals focus at startup the way SW_SHOWDEFAULT did.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // ImGui frees the draw buffers of a window it has not drawn for this long.
    // The default (60 s) keeps the settings menu's - by far the largest - alive
    // for a minute after it closes.
    io.ConfigMemoryCompactTimer = 10.0f;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice.Get(), m_pd3dDeviceContext.Get());
    m_dx11BackendReady = true;

    // A failure here is not fatal: the first frame that needs the overlay
    // retries through RecreateDevice.
    RebuildFonts(std::string());

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    m_trayTip = L"Smart Volume Mixer";
    wcscpy_s(nid.szTip, m_trayTip.c_str());
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Refreshes the tray tooltip's countdown every 5 seconds, and doubles as the
    // render loop's backstop wake. (The keyboard-hook watchdog it used to drive
    // now runs on InputHook's own thread.)
    SetTimer(m_hwnd, IDT_TRAY_REFRESH, 5000, nullptr);

    // Startup leaves a lot behind that is never touched again (the parsed
    // config, the font files, the registry font scan), and the overlay usually
    // stays hidden for a long time from here, so the first cleanup is queued
    // straight away rather than after a first hide.
    m_idleCleanupAt = GetTickCount64() + kIdleCleanupDelayMs;

    return true;
}

bool OverlayWindow::RebuildFonts(const std::string& previewName) {
    ImGuiIO& io = ImGui::GetIO();
    auto& ui = UIManager::Get();

    ImGui_ImplDX11_InvalidateDeviceObjects();
    io.Fonts->Clear();

    // Resolving a family name means a sweep of every installed font in the
    // registry, and this runs on every wake from idle - so the answer is kept.
    if (g_Config.fontFamily != m_resolvedFontFamily) {
        m_resolvedFontPath = FindFontPath(g_Config.fontFamily);
        m_resolvedFontFamily = g_Config.fontFamily;
    }
    const std::string& fontPath = m_resolvedFontPath;

    // Only the two sizes anything is actually drawn with: every face in the
    // atlas costs a full Cyrillic glyph range.
    ui.fontNormal = SafeAddFont(io, fontPath, (float)g_Config.fontSizeNormal);
    ui.fontLarge = SafeAddFont(io, fontPath, (float)g_Config.fontSizeLarge);

    ui.fontPreview = nullptr;
    if (!previewName.empty()) {
        const std::string previewPath = FindFontPath(previewName);
        if (!previewPath.empty() && fs::exists(previewPath)) {
            ui.fontPreview = io.Fonts->AddFontFromFileTTF(previewPath.c_str(), 30.0f, NULL, io.Fonts->GetGlyphRangesCyrillic());
        }
    }
    m_previewFontName = ui.fontPreview ? previewName : std::string();

    // The installed-font table has done its job. Only the settings menu's font
    // picker needs it again, and that rebuilds it on demand.
    if (!ui.IsMenuOpen()) ui.ReleaseFontScan();

    m_deviceObjectsReady = ImGui_ImplDX11_CreateDeviceObjects();
    if (m_deviceObjectsReady) {
        // The atlas now lives on the GPU. Left alone, ImGui keeps an 8-bit and
        // an expanded 32-bit copy of its pixels plus the entire TTF file behind
        // every face for the life of the process - megabytes an always-resident
        // tray app never reads again. Any later rebuild (a font preview, a lost
        // device) starts over from the font files through this function, which
        // is why nothing may call ImGui_ImplDX11_CreateDeviceObjects directly.
        io.Fonts->ClearTexData();
        io.Fonts->ClearInputData();
    }
    return m_deviceObjectsReady;
}

bool OverlayWindow::RecreateDevice() {
    // Two callers: a frame that finds no device (IdleCleanup released it), and
    // a Present that reported it lost. Everything ImGui's DX11 backend holds
    // belongs to the old device, including the device itself (it keeps a
    // reference), so the backend has to be shut down and initialised again
    // around the new one. Recreating only its objects - what the device-lost
    // path used to do - rebuilt them on the dead device.
    if (m_dx11BackendReady) {
        ImGui_ImplDX11_Shutdown();
        m_dx11BackendReady = false;
    }
    m_deviceObjectsReady = false;

    CleanupDeviceD3D();
    if (!CreateDeviceD3D()) return false;

    ImGui_ImplDX11_Init(m_pd3dDevice.Get(), m_pd3dDeviceContext.Get());
    m_dx11BackendReady = true;
    return RebuildFonts(m_previewFontName);
}

void OverlayWindow::Destroy() {
    KillTimer(m_hwnd, IDT_TRAY_REFRESH);

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);

    if (m_dx11BackendReady) {
        ImGui_ImplDX11_Shutdown();
        m_dx11BackendReady = false;
    }
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(m_hwnd);
    UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
}

// A lowered sample rate is easy to forget about, so the tray tooltip carries the
// countdown as a passive reminder. Only pushed to the shell when the text
// actually changes.
void OverlayWindow::UpdateTrayTip() {
    std::wstring tip = L"Smart Volume Mixer";

    auto& quick = QuickActions::Get();
    if (quick.IsSampleRateLowered()) {
        std::string rate = QuickActions::FormatHz(quick.CurrentSampleRate());
        long long remaining = quick.RemainingSeconds();
        std::string line = "\nAudio at " + rate;
        line += remaining >= 0 ? " - back in " + QuickActions::FormatDuration(remaining)
                               : " - no auto-restore";
        tip += std::wstring(line.begin(), line.end()); // ASCII-only by construction
    }
    if (quick.IsConsoleMode()) {
        tip += L"\nDisplay: console mode";
    }

    if (tip == m_trayTip) return;
    m_trayTip = tip;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, m_trayTip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void OverlayWindow::OnDisplayChanged() {
    if (!m_hwnd) return;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // The overlay is a fullscreen popup pinned to the desktop origin, which is
    // by definition the primary monitor's top-left corner — so a primary swap
    // means moving there. The swapchain follows on the next drawn frame
    // (EnsureSwapChain compares it against the window every frame), so
    // only an overlay that is up right now needs that frame drawn at once.
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, screenW, screenH, SWP_NOACTIVATE);
    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), 0, LWA_COLORKEY);

    if (m_isVisible) m_framesOwed = (std::max)(m_framesOwed, kSettleFrames);
}

void OverlayWindow::SetOverlayVisible(bool visible) {
    if (m_isVisible == visible || !m_hwnd) return;
    m_isVisible = visible;
    // SW_SHOWNOACTIVATE: the OSD must never pull focus out of whatever the user
    // is doing (a fullscreen game in particular).
    ShowWindow(m_hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

void OverlayWindow::RequestRedraw() {
    // The count is what makes the loop draw; the message only wakes it. Release
    // pairs with the loop's acquire load, so whatever the caller changed before
    // calling this is visible to the frame that answers it.
    m_redrawRequests.fetch_add(1, std::memory_order_release);

    // Read once: the worker thread starts before Create() runs (see the startup
    // order in main.cpp), so this can legitimately be called with no window yet.
    HWND hwnd = m_hwnd;
    if (hwnd) PostMessageW(hwnd, WM_OVERLAY_WAKE, 0, 0);
}

void OverlayWindow::UpdateClickability(bool clickable) {
    if (m_isClickable == clickable) return;
    m_isClickable = clickable;
    
    LONG_PTR exStyle = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (clickable) {
        exStyle &= ~WS_EX_TRANSPARENT;
    } else {
        exStyle |= WS_EX_TRANSPARENT;
    }
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, exStyle);
}

void OverlayWindow::RunLoop() {
    auto& ui = UIManager::Get();

    // What the frames being drawn show, and the RequestRedraw() count they
    // answer. Either one moving on means the screen is out of date.
    unsigned drawnMask = 0;
    unsigned drawnRequests = 0;
    bool done = false;

    while (!done) {
        // Idle is this app's normal state, and so is an OSD that just sits
        // there until it times out: neither needs a single frame. The loop
        // therefore blocks until something actually changes - a window message,
        // a RequestRedraw() wake, the next popup expiry - and only draws a few
        // settle frames per change instead of 60 a second for as long as
        // anything is up. Every DXGI blt-model Present copies a full-screen
        // back buffer and makes DWM recompose the whole desktop, so this is
        // most of what showing the OSD costs. The 5 s tray-refresh timer
        // doubles as a backstop should a wake ever be missed.
        {
            const ULONGLONG now = GetTickCount64();
            auto until = [now](ULONGLONG at) { return at <= now ? 0 : (DWORD)(at - now); };

            DWORD timeout = INFINITE;
            if (m_framesOwed > 0) {
                timeout = until(m_nextFrameAt);
            } else {
                if (drawnMask) timeout = ui.MsUntilNextExpiry();
                if (drawnMask & UIManager::kUiMenu) timeout = (std::min)(timeout, until(m_lastFrameAt + kMenuRefreshMs));
            }
            timeout = (std::min)(timeout, g_Config.MsUntilPendingSave());
            if (m_idleCleanupAt) timeout = (std::min)(timeout, until(m_idleCleanupAt));

            MsgWaitForMultipleObjects(0, nullptr, FALSE, timeout, QS_ALLINPUT);
        }

        // Anything but the tray timer and our own wake-up counts as input. It only
        // matters while the menu is open: the overlay is click-through otherwise.
        bool sawInput = false;
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            if (msg.message != WM_TIMER && msg.message != WM_OVERLAY_WAKE) sawInput = true;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        // Settings edits mark the config dirty instead of rewriting config.json
        // on every frame of a slider drag; this is where the write happens.
        g_Config.FlushPendingSave();

        UpdateClickability(ui.IsMenuOpen());

        if (ui.ShouldReloadPreview()) {
            const std::string previewName = ui.GetPreviewPath();
            ui.ResetReloadPreview();
            // Without a backend there is nothing to upload to; RecreateDevice
            // builds the atlas with this face once there is.
            if (m_dx11BackendReady) RebuildFonts(previewName);
            else m_previewFontName = previewName;
            m_framesOwed = (std::max)(m_framesOwed, kSettleFrames);
        }

        const unsigned mask = ui.VisibleMask();
        const unsigned requests = m_redrawRequests.load(std::memory_order_acquire);
        if (mask != drawnMask || requests != drawnRequests) {
            // The installed-font table is only for the menu's font picker,
            // which rebuilds it on demand.
            if ((drawnMask & UIManager::kUiMenu) && !(mask & UIManager::kUiMenu)) ui.ReleaseFontScan();

            drawnMask = mask;
            drawnRequests = requests;
            // Emptied: one frame clears the last popup before the window hides.
            m_framesOwed = (std::max)(m_framesOwed, mask ? kSettleFrames : 1);
        }

        const ULONGLONG now = GetTickCount64();
        if (mask & UIManager::kUiMenu) {
            // ImGui answers input a frame later (hover, clicks, popups opening),
            // and a focused text field blinks its cursor, which needs frames for
            // as long as it is focused.
            if (sawInput) m_framesOwed = (std::max)(m_framesOwed, kSettleFrames);
            if (ImGui::GetIO().WantTextInput || now - m_lastFrameAt >= kMenuRefreshMs)
                m_framesOwed = (std::max)(m_framesOwed, 1);
        }

        if (m_framesOwed > 0 && now >= m_nextFrameAt) {
            const bool hasContent = (mask != 0);
            if (!hasContent && !m_isVisible) {
                m_framesOwed = 0; // nothing on screen to clear
            } else if (RenderFrame(hasContent)) {
                --m_framesOwed;
                m_lastFrameAt = now;
                m_nextFrameAt = now + kFrameIntervalMs;
            } else {
                // No usable device. Keep owing the frames: next frame if the
                // device was just rebuilt, otherwise once the driver had time to
                // come back. A clearing frame is not worth waiting for - hiding
                // the window is enough.
                m_nextFrameAt = now + (m_deviceObjectsReady ? kFrameIntervalMs : kDeviceRetryMs);
                if (!hasContent) m_framesOwed = 0;
            }

            // Nothing left on screen. The frame just drawn was the empty one
            // that clears the last OSD, so the window can now drop out of the
            // compositor until there is something to show again.
            if (!hasContent && m_framesOwed == 0 && m_isVisible) {
                SetOverlayVisible(false);
                m_idleCleanupAt = now + kIdleCleanupDelayMs;
            }
        }

        if (m_idleCleanupAt && now >= m_idleCleanupAt) IdleCleanup();
    }
}

bool OverlayWindow::RenderFrame(bool hasContent) {
    if (!m_deviceObjectsReady && !RecreateDevice()) return false;

    // While hidden there may be no swapchain at all (IdleCleanup), and a
    // display change (resolution, or the primary swap the console hotkey
    // performs) can leave the one there is at the wrong size.
    if (!EnsureSwapChain()) return false;

    if (hasContent) {
        SetOverlayVisible(true);
        m_idleCleanupAt = 0;
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (hasContent) {
        UIManager::Get().Render();
    }

    ImGui::Render();
    const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    m_pd3dDeviceContext->OMSetRenderTargets(1, m_mainRenderTargetView.GetAddressOf(), nullptr);
    m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView.Get(), clear_color_with_alpha);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    HRESULT hr = m_pSwapChain->Present(0, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        // Device lost (driver reset or update, TDR). Rebuilding it right here
        // means the retry only has to wait one frame; if the driver is not back
        // yet the next RenderFrame tries again. Either way the app keeps running
        // - this used to end the render loop, and with it the whole process and
        // the keyboard hook.
        RecreateDevice();
        return false;
    }
    return true;
}

void OverlayWindow::IdleCleanup() {
    m_idleCleanupAt = 0;
    if (m_isVisible) return;

    // Nothing draws while the overlay is hidden, so the whole D3D device goes,
    // and with it the swapchain and everything ImGui's DX11 backend holds. The
    // device is most of what this app costs at rest. Measured on this machine
    // (AMD, 2560x1440): ~47 MB of committed memory, ~36 MB of GPU memory (the
    // back buffer, plus a window-sized present staging surface the driver
    // caches for layered windows), 7 driver threads and ~800 handles. Releasing
    // only the swapchain, or asking the driver to IDXGIDevice3::Trim(), gave
    // back the back buffer and nothing else.
    //
    // The next frame that shows something rebuilds it all (RenderFrame ->
    // RecreateDevice): ~45 ms, once per idle period. That is only acceptable
    // because the keyboard hook has its own thread now - on this thread it
    // would have held up every key press on the system for that long.
    if (m_dx11BackendReady) {
        ImGui_ImplDX11_Shutdown();
        m_dx11BackendReady = false;
    }
    m_deviceObjectsReady = false;
    CleanupDeviceD3D();

    // Hand freed heap blocks back to the OS, then drop the pages this process
    // only touches while drawing (the D3D driver, ImGui, the font atlas
    // builder). Anything touched again comes back as a cheap soft fault - the
    // keyboard hook's code included, which the next keystroke anywhere pulls
    // straight back in.
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

bool OverlayWindow::ResizeSwapChain(UINT width, UINT height) {
    if (!m_pSwapChain || !m_pd3dDeviceContext) return false;
    if (width == m_bufferW && height == m_bufferH && m_mainRenderTargetView) return true;

    // ResizeBuffers fails while anything still references the back buffer:
    // our view, and the context, which keeps it bound from the last frame.
    // ClearState drops that binding (ImGui sets its whole pipeline state anew
    // every frame), Flush lets D3D actually destroy what was released.
    CleanupRenderTarget();
    m_pd3dDeviceContext->ClearState();
    m_pd3dDeviceContext->Flush();

    const HRESULT hr = m_pSwapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                                   DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    if (FAILED(hr)) {
        // Lost device or a swapchain in a bad state - either way the next
        // frame starts over on a fresh device (RenderFrame -> RecreateDevice).
        m_bufferW = m_bufferH = 0;
        m_deviceObjectsReady = false;
        return false;
    }
    m_bufferW = width;
    m_bufferH = height;
    CreateRenderTarget();
    return m_mainRenderTargetView != nullptr;
}

bool OverlayWindow::EnsureSwapChain() {
    RECT rc;
    if (!GetClientRect(m_hwnd, &rc)) return m_mainRenderTargetView != nullptr;
    const UINT width = (UINT)(rc.right - rc.left);
    const UINT height = (UINT)(rc.bottom - rc.top);
    if (width == 0 || height == 0) return false;
    if (!m_pSwapChain) return CreateSwapChain(width, height);
    return ResizeSwapChain(width, height);
}

bool OverlayWindow::CreateSwapChain(UINT width, UINT height) {
    if (!m_pd3dDevice) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIFactory> factory;
    if (FAILED(m_pd3dDevice.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        m_deviceObjectsReady = false;
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    // One buffer: a windowed blt-model swapchain copies the back buffer into
    // the window on Present, so a second full-screen buffer would be dead
    // weight.
    sd.BufferCount = 1;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = m_hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    if (FAILED(factory->CreateSwapChain(m_pd3dDevice.Get(), &sd, &m_pSwapChain))) {
        // Same recovery as a failed resize: the next frame starts over on a
        // fresh device.
        m_deviceObjectsReady = false;
        return false;
    }
    m_bufferW = width;
    m_bufferH = height;
    CreateRenderTarget();
    return m_mainRenderTargetView != nullptr;
}

void OverlayWindow::ReleaseSwapChain() {
    if (!m_pSwapChain) return;
    // Same reasoning as in ResizeSwapChain: drop every reference to the back
    // buffer first, and Flush so it is actually destroyed now - a window can
    // only have one swapchain, and the next one may be created soon.
    CleanupRenderTarget();
    if (m_pd3dDeviceContext) {
        m_pd3dDeviceContext->ClearState();
        m_pd3dDeviceContext->Flush();
    }
    m_pSwapChain.Reset();
    m_bufferW = m_bufferH = 0;
}

bool OverlayWindow::CreateDeviceD3D() {
    // Device only. The swapchain - by far the bigger allocation - exists only
    // while there is something on screen (EnsureSwapChain / IdleCleanup).
    //
    // The device and its context are only ever touched from this thread (the
    // render loop and the ImGui DX11 backend it drives), so D3D can skip the
    // internal locking it otherwise takes on every single call.
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_SINGLETHREADED;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext) == S_OK;
}

void OverlayWindow::CleanupDeviceD3D() {
    ReleaseSwapChain();
    if (m_pd3dDeviceContext) {
        // Unbind and submit everything first, so the driver can tear the
        // device down at once instead of on its deferred-destruction path.
        m_pd3dDeviceContext->ClearState();
        m_pd3dDeviceContext->Flush();
    }
    m_pd3dDeviceContext.Reset();
    m_pd3dDevice.Reset();
}

void OverlayWindow::CreateRenderTarget() {
    if (!m_pSwapChain || !m_pd3dDevice) return;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pBackBuffer;
    if (FAILED(m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) || !pBackBuffer) return;
    m_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &m_mainRenderTargetView);
}

void OverlayWindow::CleanupRenderTarget() {
    m_mainRenderTargetView.Reset();
}

LRESULT WINAPI OverlayWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    if (msg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);

            auto& quick = QuickActions::Get();
            std::wstring rateItem;
            if (quick.IsSampleRateLowered()) {
                std::string label = "Restore audio to " +
                    QuickActions::FormatHz(g_Config.srSavedHz > 0 ? g_Config.srSavedHz : g_Config.srHighHz);
                long long remaining = quick.RemainingSeconds();
                if (remaining >= 0) label += " (auto in " + QuickActions::FormatDuration(remaining) + ")";
                rateItem.assign(label.begin(), label.end()); // ASCII-only by construction
            } else {
                std::string label = "Lower audio to " + QuickActions::FormatHz(g_Config.srLowHz);
                rateItem.assign(label.begin(), label.end());
            }

            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"Settings");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 3, rateItem.c_str());
            AppendMenuW(hMenu, MF_STRING, 4, quick.IsConsoleMode() ? L"Display: back to PC" : L"Display: switch to console");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, 2, L"Exit");
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);
            if (cmd == 1) {
                UIManager::Get().SetMenuOpen(true);
            } else if (cmd == 2) {
                PostQuitMessage(0);
            } else if (cmd == 3 || cmd == 4) {
                // Hand the actual work to the MTA worker — the tray menu runs on
                // the UI thread, which draws the OSD (invariant 8).
                auto& hook = InputHook::Get();
                std::lock_guard<std::mutex> lock(hook.eventMutex);
                hook.pendingEvents.push_back({ cmd == 3 ? 5 : 6, false, false, -1, 0 });
                hook.eventCv.notify_one();
            }
        } else if (lParam == WM_LBUTTONDBLCLK) {
            UIManager::Get().SetMenuOpen(true);
        }
        return 0;
    }

    switch (msg) {
        case WM_OVERLAY_WAKE:
            // Nothing to do: pumping it is the whole point (see RequestRedraw).
            return 0;
        case WM_TIMER:
            if (wParam == IDT_TRAY_REFRESH) {
                OverlayWindow::Get().UpdateTrayTip();
            }
            return 0;
        case WM_DISPLAYCHANGE:
            OverlayWindow::Get().OnDisplayChanged();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
