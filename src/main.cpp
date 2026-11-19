#include "Config.h"
#include "AudioManager.h"
#include "InputHook.h"
#include "UIManager.h"
#include "OverlayWindow.h"
#include "QuickActions.h"
#include "Perf.h"
#include <windows.h>

// Relaunch ourselves. Used both by the per-thread crash filter and the
// main-thread SEH guard so an unhandled fault never leaves the keyboard hook
// dead — a dead hook means VK_VOLUME_MUTE reverts to the OS default and mutes
// the entire system instead of toggling our media apps.
static void RestartSelf() {
    wchar_t szPath[1024];
    const DWORD cap = (DWORD)(sizeof(szPath) / sizeof(szPath[0]));
    const DWORD len = GetModuleFileNameW(NULL, szPath, cap);
    if (len > 0 && len < cap) {
        ShellExecuteW(NULL, L"open", szPath, NULL, NULL, SW_SHOW);
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
    PERF_LOG("S process_start_to_appmain %.3f", Perf::MsSinceProcessStart());
    {
        PERF_SCOPE("start.Config.Load");
        g_Config.Load();
    }

    // Picks up a sample-rate change that was still pending when the previous
    // instance ended (clean exit or crash-relaunch) and re-arms its deadline, so
    // the DAC can never stay at the game rate just because we were restarted.
    QuickActions::Get().LoadPersistedState();

    bool audioOk;
    {
        PERF_SCOPE("start.AudioManager.Initialize");
        audioOk = AudioManager::Get().Initialize();
    }
    if (!audioOk) {
        MessageBoxW(nullptr, L"Failed to initialize AudioManager (COM / WASAPI)", L"Error", MB_ICONERROR);
        return -1;
    }

    UIManager::Get().Initialize();

    bool hookOk;
    {
        PERF_SCOPE("start.InputHook.Install");
        hookOk = InputHook::Get().Install();
    }
    if (!hookOk) {
        MessageBoxW(nullptr, L"Failed to install keyboard hook", L"Error", MB_ICONERROR);
        AudioManager::Get().Shutdown();
        return -1;
    }
    PERF_LOG("S process_start_to_hook %.3f", Perf::MsSinceProcessStart());

    bool overlayOk;
    {
        PERF_SCOPE("start.OverlayWindow.Create");
        overlayOk = OverlayWindow::Get().Create();
    }
    if (!overlayOk) {
        MessageBoxW(nullptr, L"Failed to create overlay window", L"Error", MB_ICONERROR);
        InputHook::Get().Uninstall();
        AudioManager::Get().Shutdown();
        return -1;
    }
    PERF_LOG("S process_start_to_loop %.3f", Perf::MsSinceProcessStart());

    OverlayWindow::Get().RunLoop();

    OverlayWindow::Get().Destroy();
    InputHook::Get().Uninstall();
    AudioManager::Get().Shutdown();

    // A settings edit made in the last moments before Exit is still only a
    // dirty flag at this point (see AppConfig::SaveDeferred).
    g_Config.FlushPendingSave(true);

    PERF_ONLY(Perf::DumpCounters(); Perf::Write("E exit");)
    PERF_FLUSH();

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
