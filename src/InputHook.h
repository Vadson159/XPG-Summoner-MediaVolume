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

struct HookEvent {
    int type; // 0 = volume, 1 = play/pause, 2 = toggle menu, 3 = track info
    bool volUp;
    bool volLargeStep;
};

class InputHook {
public:
    static InputHook& Get();

    bool Install();
    void Uninstall();
    void ReinstallHook(); // Called periodically to guard against Windows removing the hook

    // Callbacks to notify UI
    std::function<void(float vol, const std::string& appName)> OnVolumeChanged;
    std::function<void()> OnMenuToggled;
    std::function<void(const std::string& trackName)> OnTrackInfoRequested;
    
    // Event queue to prevent hook timeout
    std::mutex eventMutex;
    std::condition_variable eventCv;
    std::vector<HookEvent> pendingEvents;
    
    std::thread m_workerThread;
    std::atomic<bool> m_stopWorker = false;
    void WorkerLoop();

    // State for capturing key
    bool isCapturingKey = false;
    int captureType = 0; // 0 = Volume Modifier, 1 = Menu Key, 2 = Info Key
    std::function<void(int vkCode, int type)> OnKeyCaptured;

private:
    InputHook() = default;
    ~InputHook() = default;

    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);

    HHOOK m_hook = nullptr;
};
