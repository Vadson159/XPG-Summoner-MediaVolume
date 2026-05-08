#include "Config.h"
#include "AudioManager.h"
#include "InputHook.h"
#include "UIManager.h"
#include "OverlayWindow.h"
#include <windows.h>

static int AppMain() {
    g_Config.Load();

    if (!AudioManager::Get().Initialize()) {
        MessageBoxW(nullptr, L"Failed to initialize AudioManager (COM / WASAPI)", L"Error", MB_ICONERROR);
        return -1;
    }

    UIManager::Get().Initialize();

    if (!InputHook::Get().Install()) {
        MessageBoxW(nullptr, L"Failed to install keyboard hook", L"Error", MB_ICONERROR);
        AudioManager::Get().Shutdown();
        return -1;
    }

    if (!OverlayWindow::Get().Create()) {
        MessageBoxW(nullptr, L"Failed to create overlay window", L"Error", MB_ICONERROR);
        InputHook::Get().Uninstall();
        AudioManager::Get().Shutdown();
        return -1;
    }

    OverlayWindow::Get().RunLoop();

    OverlayWindow::Get().Destroy();
    InputHook::Get().Uninstall();
    AudioManager::Get().Shutdown();

    return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    __try {
        return AppMain();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Restart the application on unhandled crash
        char szPath[MAX_PATH];
        GetModuleFileNameA(NULL, szPath, MAX_PATH);
        ShellExecuteA(NULL, "open", szPath, NULL, NULL, SW_SHOW);
        return -1;
    }
}
