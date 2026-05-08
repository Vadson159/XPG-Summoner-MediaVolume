#include "OverlayWindow.h"
#include "UIManager.h"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>
#include <dxgi.h>
#include "resource.h"
#include "InputHook.h"
#include "Config.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#include <shlobj.h>
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

    if (UIManager::Get().systemFonts.count(fontName)) {
        std::string file = UIManager::Get().systemFonts.at(fontName);
        
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
#define IDT_HOOK_WATCHDOG 42

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

    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(m_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_pd3dDevice.Get(), m_pd3dDeviceContext.Get());

    // Load Fonts
    std::string fontPath = FindFontPath(g_Config.fontFamily);
    const char* monoPath = "C:\\Windows\\Fonts\\consola.ttf";
    
    UIManager::Get().fontNormal = SafeAddFont(io, fontPath, (float)g_Config.fontSizeNormal);
    UIManager::Get().fontLarge = SafeAddFont(io, fontPath, (float)g_Config.fontSizeLarge);
    UIManager::Get().fontSmall = SafeAddFont(io, fontPath, (float)g_Config.fontSizeSmall);
    UIManager::Get().fontMono = SafeAddFont(io, monoPath, (float)g_Config.fontSizeNormal);


    ImGui_ImplDX11_CreateDeviceObjects();


    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = (HICON)LoadImage(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wcscpy_s(nid.szTip, L"Smart Volume Mixer");
    Shell_NotifyIconW(NIM_ADD, &nid);

    // Watchdog timer: reinstall keyboard hook every 5 seconds
    SetTimer(m_hwnd, IDT_HOOK_WATCHDOG, 5000, nullptr);

    return true;
}

void OverlayWindow::Destroy() {
    KillTimer(m_hwnd, IDT_HOOK_WATCHDOG);

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(m_hwnd);
    UnregisterClassW(m_wc.lpszClassName, m_wc.hInstance);
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
    bool done = false;
    bool wasUIActive = false;
    while (!done) {
        // Wait for messages OR timeout — this keeps the hook alive
        // by ensuring we pump messages immediately when they arrive
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);

        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done) break;

        UpdateClickability(UIManager::Get().IsMenuOpen());

        if (UIManager::Get().ShouldReloadPreview()) {
            std::string previewPath = UIManager::Get().GetPreviewPath();
            ImGuiIO& io = ImGui::GetIO();
            
            // Re-load everything
            ImGui_ImplDX11_InvalidateDeviceObjects();
            io.Fonts->Clear();
            
            std::string fontPath = FindFontPath(g_Config.fontFamily);
            const char* monoPath = "C:\\Windows\\Fonts\\consola.ttf";

            UIManager::Get().fontNormal = SafeAddFont(io, fontPath, (float)g_Config.fontSizeNormal);
            UIManager::Get().fontLarge = SafeAddFont(io, fontPath, (float)g_Config.fontSizeLarge);
            UIManager::Get().fontSmall = SafeAddFont(io, fontPath, (float)g_Config.fontSizeSmall);
            UIManager::Get().fontMono = SafeAddFont(io, monoPath, (float)g_Config.fontSizeNormal);
            
            // Resolve preview path using the same helper
            std::string actualPreviewPath = FindFontPath(previewPath);
            if (!actualPreviewPath.empty() && fs::exists(actualPreviewPath)) {
                UIManager::Get().fontPreview = io.Fonts->AddFontFromFileTTF(actualPreviewPath.c_str(), 30.0f, NULL, io.Fonts->GetGlyphRangesCyrillic());
            } else {
                UIManager::Get().fontPreview = nullptr;
            }

            ImGui_ImplDX11_CreateDeviceObjects();
            UIManager::Get().ResetReloadPreview();
        }

        bool isUIActive = UIManager::Get().HasActiveUI();
        if (isUIActive || wasUIActive) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            if (isUIActive) {
                UIManager::Get().Render();
            }

            ImGui::Render();
            const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            m_pd3dDeviceContext->OMSetRenderTargets(1, m_mainRenderTargetView.GetAddressOf(), nullptr);
            m_pd3dDeviceContext->ClearRenderTargetView(m_mainRenderTargetView.Get(), clear_color_with_alpha);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

            HRESULT hr = m_pSwapChain->Present(0, 0);
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
                // Device lost — recreate everything
                ImGui_ImplDX11_InvalidateDeviceObjects();
                CleanupRenderTarget();
                CleanupDeviceD3D();
                if (!CreateDeviceD3D()) {
                    done = true;
                    break;
                }
                CreateRenderTarget();
                ImGui_ImplDX11_CreateDeviceObjects();
            }
        }
        wasUIActive = isUIActive;
    }
}

bool OverlayWindow::CreateDeviceD3D() {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
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

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &m_pSwapChain, &m_pd3dDevice, &featureLevel, &m_pd3dDeviceContext) != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void OverlayWindow::CleanupDeviceD3D() {
    CleanupRenderTarget();
    m_pSwapChain.Reset();
    m_pd3dDeviceContext.Reset();
    m_pd3dDevice.Reset();
}

void OverlayWindow::CreateRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pBackBuffer;
    m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
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
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"Settings");
            AppendMenuW(hMenu, MF_STRING, 2, L"Exit");
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);
            if (cmd == 1) {
                UIManager::Get().SetMenuOpen(true);
            } else if (cmd == 2) {
                PostQuitMessage(0);
            }
        } else if (lParam == WM_LBUTTONDBLCLK) {
            UIManager::Get().SetMenuOpen(true);
        }
        return 0;
    }

    switch (msg) {
        case WM_TIMER:
            if (wParam == IDT_HOOK_WATCHDOG) {
                InputHook::Get().ReinstallHook();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
