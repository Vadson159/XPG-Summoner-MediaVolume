#pragma once
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

class OverlayWindow {
public:
    static OverlayWindow& Get();

    bool Create();
    void Destroy();
    void RunLoop();

    void UpdateClickability(bool clickable);

private:
    OverlayWindow() = default;
    ~OverlayWindow() = default;

    HWND m_hwnd = nullptr;
    WNDCLASSEXW m_wc = {};
    bool m_isClickable = false;

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
