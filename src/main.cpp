#include "Config.h"
#include "AudioManager.h"
#include "InputHook.h"
#include "UIManager.h"
#include "OverlayWindow.h"
#include "QuickActions.h"
#include <windows.h>

// Relaunch ourselves. Used both by the per-thread crash filter and the
// main-thread SEH guard so an unhandled fault never leaves the keyboard hook
// dead — a dead hook means VK_VOLUME_MUTE reverts to the OS default and mutes
// the entire system instead of toggling our media apps.
static void RestartSelf() {
    char szPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szPath, MAX_PATH) > 0) {
        ShellExecuteA(NULL, "open", szPath, NULL, NULL, SW_SHOW);
    }
}

// Catches faults on ANY thread (the worker thread in particular). The
// __try/__except in wWinMain only guards the main thread, so without this a
// crash inside the audio worker would kill the process with the hook still
// installed and never restart.
static LONG WINAPI GlobalCrashHandler(EXCEPTION_POINTERS*) {
    RestartSelf();
    return EXCEPTION_EXECUTE_HANDLER; // terminate this (now-relaunched) instance
}

static int AppMain() {
    g_Config.Load();

    // Picks up a sample-rate change that was still pending when the previous
    // instance ended (clean exit or crash-relaunch) and re-arms its deadline, so
    // the DAC can never stay at the game rate just because we were restarted.
    QuickActions::Get().LoadPersistedState();

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

    // A settings edit made in the last moments before Exit is still only a
    // dirty flag at this point (see AppConfig::SaveDeferred).
    g_Config.FlushPendingSave(true);

    return 0;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    // Process-wide net: relaunches us on a fault originating on any thread.
    SetUnhandledExceptionFilter(GlobalCrashHandler);

    __try {
        return AppMain();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Restart the application on unhandled crash (main thread)
        RestartSelf();
        return -1;
    }
}
