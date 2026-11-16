#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include "MuteHdrHotkey.h"

struct HookEvent {
    // 0 = volume, 1 = play/pause, 2 = toggle menu, 3 = track info,
    // 4 = per-app mute toggle, 5 = sample-rate action, 6 = display-swap action,
    // 7 = rescan monitors, 8 = toggle Windows HDR, 9 = toggle monitor HDR Effect
    int type;
    bool volUp;
    bool volLargeStep;
    int bindingIndex = -1; // -1 = normal mode, >=0 = index into g_Config.appBindings
    int actionArg = 0;     // types 5/6: 0 = toggle, 1 = force off/PC, 2 = force on/console
    // types 0/1: route to g_Config.secondaryApps (the browsers) instead of
    // g_Config.mediaApps, because the secondary key was held when the key fired.
    bool secondaryGroup = false;
};

class InputHook {
public:
    static InputHook& Get();

    // Starts the hook thread (which installs WH_KEYBOARD_LL and pumps its
    // messages) and then the worker. Returns false if the hook could not be
    // installed; nothing is left running in that case.
    bool Install();
    void Uninstall();
    void SetHdrHotkeys(HdrHotkey windows, HdrHotkey monitor) {
        m_hdrHotkeys.store(windows.Pack() | (monitor.Pack() << 16), std::memory_order_relaxed);
    }

    // Callbacks to notify UI
    std::function<void(float vol, const std::string& appName)> OnVolumeChanged;
    std::function<void()> OnMenuToggled;
    std::function<void(const std::string& trackName)> OnTrackInfoRequested;
    std::function<void(bool muted, const std::string& appName, float vol)> OnAppMuteToggled;
    
    // Event queue to prevent hook timeout
    std::mutex eventMutex;
    std::condition_variable eventCv;
    std::vector<HookEvent> pendingEvents;
    
    std::thread m_workerThread;
    std::atomic<bool> m_stopWorker = false;
    void WorkerLoop();

    // Key capture for the settings menu. The render thread arms it, the hook
    // thread answers the next key press through OnKeyCaptured (on the hook
    // thread) and disarms it.
    // Type: 0 = Volume Modifier, 1 = Menu Key, 2 = Info Key, 3 = per-app binding
    // modifier, 4 = Sample Rate Key, 5 = Display Swap Key, 6 = browser-group
    // (secondary) key, 7 = Windows HDR key, 8 = monitor HDR Effect key.
    // NOTE: this numbering is NOT the same space as HookEvent::type above.
    // Types 0, 3 and 6 capture a held modifier (so Ctrl/Shift/Alt are valid);
    // every other type captures a hotkey and skips modifier-only presses.
    void BeginKeyCapture(int type) {
        captureType.store(type, std::memory_order_relaxed);
        isCapturingKey.store(true, std::memory_order_release); // publishes the type
    }
    std::atomic<bool> isCapturingKey{false};
    std::atomic<int> captureType{0};
    std::function<void(int vkCode, int type)> OnKeyCaptured;

private:
    InputHook() = default;
    ~InputHook() = default;

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    // The hook lives on a thread of its own that does nothing but pump the
    // messages that drive it - see HookThreadMain.
    void HookThreadMain(std::promise<bool> installed);
    void ReinstallHook(); // hook thread only: guards against Windows removing the hook

    std::thread m_hookThread;
    DWORD m_hookThreadId = 0;
    HHOOK m_hook = nullptr; // hook thread only
    MuteHdrHotkey m_muteHdrHotkey;
    std::atomic<uint32_t> m_hdrHotkeys{0}; // publish both bindings as one coherent snapshot
    HookEvent m_pendingMuteEvent{1, false, false}; // route captured on Mute down
};
