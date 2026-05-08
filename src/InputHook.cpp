#include "InputHook.h"
#include "Config.h"
#include "AudioManager.h"

InputHook& InputHook::Get() {
    static InputHook instance;
    return instance;
}

bool InputHook::Install() {
    if (m_hook) return true;
    m_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
    if (m_hook) {
        m_stopWorker = false;
        m_workerThread = std::thread(&InputHook::WorkerLoop, this);
    }
    return m_hook != nullptr;
}

void InputHook::Uninstall() {
    if (m_hook) {
        UnhookWindowsHookEx(m_hook);
        m_hook = nullptr;
    }
    m_stopWorker = true;
    eventCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void InputHook::ReinstallHook() {
    HHOOK newHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
    if (newHook) {
        HHOOK oldHook = m_hook;
        m_hook = newHook;
        if (oldHook) {
            UnhookWindowsHookEx(oldHook);
        }
    }
}

LRESULT CALLBACK InputHook::LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKbd = (KBDLLHOOKSTRUCT*)lParam;
        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

        if (isKeyDown) {
            auto& hook = InputHook::Get();

            if (pKbd->vkCode == VK_VOLUME_MUTE) {
                std::lock_guard<std::mutex> lock(hook.eventMutex);
                hook.pendingEvents.push_back({1, false, false});
                hook.eventCv.notify_one();
                return 1;
            }

            if (hook.isCapturingKey) {
                int vk = pKbd->vkCode;
                if (vk == VK_VOLUME_UP || vk == VK_VOLUME_DOWN || vk == VK_VOLUME_MUTE ||
                    vk == VK_MEDIA_PLAY_PAUSE || vk == VK_MEDIA_NEXT_TRACK || vk == VK_MEDIA_PREV_TRACK ||
                    vk == VK_MEDIA_STOP) {
                    return CallNextHookEx(InputHook::Get().m_hook, nCode, wParam, lParam);
                }
                if (hook.captureType == 1 || hook.captureType == 2) {
                    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                        vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
                        return CallNextHookEx(InputHook::Get().m_hook, nCode, wParam, lParam);
                    }
                }
                hook.isCapturingKey = false;
                if (hook.OnKeyCaptured) hook.OnKeyCaptured(vk, hook.captureType);
                return 1;
            }

            if (pKbd->vkCode == (DWORD)g_Config.menuKey) {
                bool ok = true;
                if (g_Config.menuCtrl && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) ok = false;
                if (g_Config.menuShift && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) ok = false;
                if (g_Config.menuAlt && !(GetAsyncKeyState(VK_MENU) & 0x8000)) ok = false;
                if (ok) {
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({2, false, false});
                    hook.eventCv.notify_one();
                    return 1;
                }
            }

            if (g_Config.infoKey != 0 && pKbd->vkCode == (DWORD)g_Config.infoKey) {
                bool ok = true;
                if (g_Config.infoCtrl && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) ok = false;
                if (g_Config.infoShift && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) ok = false;
                if (g_Config.infoAlt && !(GetAsyncKeyState(VK_MENU) & 0x8000)) ok = false;
                if (ok) {
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({3, false, false});
                    hook.eventCv.notify_one();
                    return 1;
                }
            }

            if (pKbd->vkCode == VK_VOLUME_UP || pKbd->vkCode == VK_VOLUME_DOWN) {
                bool modifierPressed = false;
                if (g_Config.modifierKey == 0) {
                    modifierPressed = true;
                } else {
                    modifierPressed = (GetAsyncKeyState(g_Config.modifierKey) & 0x8000) != 0;
                }

                if (modifierPressed) {
                    bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    bool up = (pKbd->vkCode == VK_VOLUME_UP);
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({0, up, isShift});
                    hook.eventCv.notify_one();
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void InputHook::WorkerLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    int accumulatedTicks = 0;
    bool debounceActive = false;
    bool isStreaming = false;
    std::chrono::steady_clock::time_point debounceStart;
    std::chrono::steady_clock::time_point lastStreamTick;
    const int streamTimeoutMs = 500; // Return to idle after 500ms of no activity

    while (!m_stopWorker) {
        std::vector<HookEvent> events;
        {
            std::unique_lock<std::mutex> lock(eventMutex);

            auto now = std::chrono::steady_clock::now();
            auto deadline = now + std::chrono::hours(1); // Default far away

            if (debounceActive) {
                deadline = (std::min)(deadline, debounceStart + std::chrono::milliseconds(g_Config.volDebounceWindowMs));
            }
            if (isStreaming) {
                deadline = (std::min)(deadline, lastStreamTick + std::chrono::milliseconds(streamTimeoutMs));
            }

            if (debounceActive || isStreaming) {
                eventCv.wait_until(lock, deadline, [this]() {
                    return m_stopWorker || !pendingEvents.empty();
                });
            } else {
                eventCv.wait(lock, [this]() { return m_stopWorker || !pendingEvents.empty(); });
            }

            if (m_stopWorker) break;
            events = pendingEvents;
            pendingEvents.clear();
        }

        auto now = std::chrono::steady_clock::now();

        // Process timeouts first
        if (isStreaming && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStreamTick).count() >= streamTimeoutMs) {
            isStreaming = false;
        }
        if (debounceActive && std::chrono::duration_cast<std::chrono::milliseconds>(now - debounceStart).count() >= g_Config.volDebounceWindowMs) {
            debounceActive = false;
            accumulatedTicks = 0;
        }

        for (const auto& ev : events) {
            if (ev.type == 1) {
                AudioManager::Get().TogglePlayPause();
            } else if (ev.type == 2) {
                if (OnMenuToggled) OnMenuToggled();
            } else if (ev.type == 3) {
                std::string track = AudioManager::Get().GetCurrentTrackInfo();
                if (OnTrackInfoRequested) OnTrackInfoRequested(track);
            } else if (ev.type == 0) {
                if (ev.volLargeStep) {
                    isStreaming = true;
                    debounceActive = false;
                    lastStreamTick = now;
                    accumulatedTicks = 0;

                    float newVol = 0.0f;
                    std::string appName;
                    if (AudioManager::Get().ChangeVolume(ev.volUp, true, newVol, appName)) {
                        if (OnVolumeChanged) OnVolumeChanged(newVol, appName);
                    }
                } else {
                    if (isStreaming) {
                        lastStreamTick = now;
                        float newVol = 0.0f;
                        std::string appName;
                        if (AudioManager::Get().ChangeVolume(ev.volUp, false, newVol, appName)) {
                            if (OnVolumeChanged) OnVolumeChanged(newVol, appName);
                        }
                    } else if (debounceActive) {
                        accumulatedTicks += ev.volUp ? 1 : -1;
                        int threshold = (std::max)(1, g_Config.volDebounceThreshold);
                        if (std::abs(accumulatedTicks) >= threshold) {
                            bool up = accumulatedTicks > 0;
                            int count = std::abs(accumulatedTicks);
                            for (int i = 0; i < count; i++) {
                                float newVol = 0.0f;
                                std::string appName;
                                if (AudioManager::Get().ChangeVolume(up, false, newVol, appName)) {
                                    if (OnVolumeChanged) OnVolumeChanged(newVol, appName);
                                }
                            }
                            isStreaming = true;
                            debounceActive = false;
                            lastStreamTick = now;
                            accumulatedTicks = 0;
                        }
                    } else {
                        // Start debounce
                        debounceActive = true;
                        debounceStart = now;
                        accumulatedTicks = ev.volUp ? 1 : -1;
                        
                        if (g_Config.volDebounceThreshold <= 1) {
                            float newVol = 0.0f;
                            std::string appName;
                            if (AudioManager::Get().ChangeVolume(ev.volUp, false, newVol, appName)) {
                                if (OnVolumeChanged) OnVolumeChanged(newVol, appName);
                            }
                            isStreaming = true;
                            debounceActive = false;
                            lastStreamTick = now;
                            accumulatedTicks = 0;
                        }
                    }
                }
            }
        }

        // Now check for timeouts for the NEXT iteration
        now = std::chrono::steady_clock::now();
        if (isStreaming && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStreamTick).count() >= streamTimeoutMs) {
            isStreaming = false;
        }
        if (debounceActive && std::chrono::duration_cast<std::chrono::milliseconds>(now - debounceStart).count() >= (std::max)(10, g_Config.volDebounceWindowMs)) {
            debounceActive = false;
            accumulatedTicks = 0;
        }
    }

    CoUninitialize();
}
