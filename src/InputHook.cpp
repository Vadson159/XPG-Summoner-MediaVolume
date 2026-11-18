#include "InputHook.h"
#include "Config.h"
#include "AudioManager.h"
#include "QuickActions.h"
#include "Perf.h"

// --- Worker-thread crash isolation -----------------------------------------
// The worker thread drives COM/WinRT audio APIs. A stale audio-session pointer
// can raise a hardware exception (access violation) which /EHsc cannot catch
// with catch(...). If that propagates it kills the whole process WITH the
// keyboard hook still notionally owned by us, so the mute key stops being
// intercepted and Windows mutes the entire system. These thin SEH wrappers
// contain the fault: the bad call returns "no-op" and the thread survives.
//
// SEH (__try/__except) cannot live in a function that also unwinds C++ objects,
// so each wrapper is standalone and owns no locals with destructors.
static bool SafeChangeVolume(bool up, bool largeStep, float& outVol, std::string* outName, bool secondaryGroup,
                             int steps = 1) {
    __try {
        return AudioManager::Get().ChangeVolume(up, largeStep, outVol, *outName, secondaryGroup, steps);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Inner functions do the C++ work (std::string construction needs object unwinding,
// which cannot share a frame with __try under /EHsc — MSVC C2712).
static bool ChangeVolumeForAppInner(const char* appName, bool up, bool largeStep, float& outVol, int steps) {
    std::string app(appName);
    return AudioManager::Get().ChangeVolumeForApp(app, up, largeStep, outVol, steps);
}

static bool SafeChangeVolumeForApp(const char* appName, bool up, bool largeStep, float& outVol, int steps = 1) {
    __try {
        return ChangeVolumeForAppInner(appName, up, largeStep, outVol, steps);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void SafeTogglePlayPause(bool secondaryGroup) {
    __try {
        AudioManager::Get().TogglePlayPause(secondaryGroup);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static bool TogglePlayPauseForAppInner(const char* appName) {
    std::string app(appName);
    return AudioManager::Get().TogglePlayPauseForApp(app);
}

static bool SafeTogglePlayPauseForApp(const char* appName) {
    __try {
        return TogglePlayPauseForAppInner(appName);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Standalone inner function: the std::string assignment needs object unwinding,
// which cannot share a frame with __try under /EHsc (MSVC C2712).
static void ToggleMuteForAppInner(const char* appName, bool* outMuted, float* outVol, bool* outFound) {
    std::string app(appName);
    *outFound = AudioManager::Get().ToggleMuteForApp(app, *outMuted, *outVol);
}

static bool SafeToggleMuteForApp(const char* appName, bool& outMuted, float& outVol) {
    __try {
        bool found = false;
        ToggleMuteForAppInner(appName, &outMuted, &outVol, &found);
        return found;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Inner does the C++ work (assigning a returned std::string needs object
// unwinding, which cannot share a frame with __try under /EHsc — MSVC C2712),
// the wrapper provides the SEH frame around it.
static void GetTrackInfoInner(std::string* out) {
    *out = AudioManager::Get().GetCurrentTrackInfo();
}

static bool SafeGetTrackInfo(std::string* out) {
    __try {
        GetTrackInfoInner(out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The quick actions drive IPolicyConfig, the DDC/CI monitor APIs and
// ChangeDisplaySettingsEx — all of which end up in third-party driver code, so
// they get the same SEH treatment as the audio calls above.
static void SafeSampleRateAction(int arg) {
    __try {
        QuickActions::Get().SampleRateAction(arg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void SafeDisplayAction(int arg) {
    __try {
        QuickActions::Get().DisplayAction(arg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void SafeRefreshMonitors() {
    __try {
        QuickActions::Get().RefreshMonitors();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void SafeHdrAction() {
    __try {
        QuickActions::Get().HdrAction();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void SafeMonitorHdrEffectAction() {
    __try {
        QuickActions::Get().MonitorHdrEffectAction();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Tag our replay/masking events so they pass through without re-entering the
// chord state machine or triggering a configured application hotkey.
static constexpr ULONG_PTR kHotkeyReplayTag = 0x534D5848;

static void SendHotkeyReplay(WORD key, WORD forwardKey = 0, bool forwardDown = false) {
    INPUT inputs[3] = {};
    for (auto& input : inputs) {
        input.type = INPUT_KEYBOARD;
        input.ki.dwExtraInfo = kHotkeyReplayTag;
    }
    inputs[0].ki.wVk = key;
    inputs[1].ki.wVk = key;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[2].ki.wVk = forwardKey;
    inputs[2].ki.dwFlags = forwardDown ? 0 : KEYEVENTF_KEYUP;
    SendInput(forwardKey ? 3 : 2, inputs, sizeof(INPUT));
}

static void SafeProcessTimers() {
    __try {
        QuickActions::Get().ProcessTimers();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void SafeQuickActionsShutdown() {
    __try {
        QuickActions::Get().OnShutdown();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Releases the WASAPI objects this thread cached, while its MTA apartment is
// still alive. Must run before CoUninitialize below.
static void SafeReleaseAudioThreadCache() {
    __try {
        AudioManager::ReleaseThreadCache();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// --- Browser-group gate ----------------------------------------------------
// The roller and the Mute key only ever reach g_Config.mediaApps (Spotify, a
// DAW, a player). A browser is reached by holding g_Config.secondaryKey - the
// Pause key by default - which routes the same tick to g_Config.secondaryApps
// instead.
//
// "Held" cannot be answered by GetAsyncKeyState alone: Pause/Break reports its
// key-up the instant it is pressed on most keyboards, so it never registers as
// down. The hook therefore stamps every press of that key below and treats it
// as still held for secondaryHoldMs, refreshed by each tick it routes so a long
// roll does not fall back to the audio apps halfway through.
static std::atomic<ULONGLONG> g_secondaryDownTick{0};

static bool SecondaryGroupActive() {
    const int key = g_Config.secondaryKey;
    if (key == 0) return false;

    const ULONGLONG now = GetTickCount64();
    if (GetAsyncKeyState(key) & 0x8000) {
        g_secondaryDownTick.store(now, std::memory_order_relaxed);
        return true;
    }

    const ULONGLONG last = g_secondaryDownTick.load(std::memory_order_relaxed);
    if (last == 0) return false;
    const int windowMs = g_Config.secondaryHoldMs;
    if (windowMs <= 0 || now - last > (ULONGLONG)windowMs) return false;

    g_secondaryDownTick.store(now, std::memory_order_relaxed);
    return true;
}

// Required modifiers must be held; extra ones are ignored, matching how the
// menu and track-info hotkeys have always behaved. Alt means left Alt only,
// leaving right Alt / AltGr available to the focused application.
static bool ModifiersHeld(bool needCtrl, bool needShift, bool needAlt) {
    if (needCtrl && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) return false;
    if (needShift && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) return false;
    if (needAlt && !(GetAsyncKeyState(VK_LMENU) & 0x8000)) return false;
    return true;
}

// Two volume ticks that would do exactly the same thing, so a run of them can
// be applied as one multi-step change (see WorkerLoop).
static bool SameVolumeTick(const HookEvent& a, const HookEvent& b) {
    return a.type == 0 && b.type == 0 &&
           a.volUp == b.volUp && a.volLargeStep == b.volLargeStep &&
           a.bindingIndex == b.bindingIndex && a.secondaryGroup == b.secondaryGroup;
}
// ---------------------------------------------------------------------------

InputHook& InputHook::Get() {
    static InputHook instance;
    return instance;
}

// How often the hook thread re-registers the hook, in case Windows dropped it.
static const UINT kHookWatchdogMs = 5000;

bool InputHook::Install() {
    if (m_hookThread.joinable()) return true;
    SetHdrHotkeys(g_Config.windowsHdrHotkey, g_Config.monitorHdrHotkey);

    std::promise<bool> installed;
    std::future<bool> result = installed.get_future();
    m_hookThread = std::thread(&InputHook::HookThreadMain, this, std::move(installed));
    if (!result.get()) {
        m_hookThread.join();
        return false;
    }

    m_stopWorker = false;
    m_workerThread = std::thread(&InputHook::WorkerLoop, this);
    return true;
}

void InputHook::Uninstall() {
    if (m_hookThread.joinable()) {
        PostThreadMessageW(m_hookThreadId, WM_QUIT, 0, 0);
        m_hookThread.join();
    }
    m_stopWorker = true;
    eventCv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

// A low-level hook is called on the thread that installed it, and only while
// that thread pumps messages; every key press anywhere on the system waits for
// it. It used to share the overlay's render thread, so each frame, font
// rebuild or D3D device creation held up the whole system's keyboard for its
// duration - and anything slow enough risked Windows silently removing the hook
// past LowLevelHooksTimeout, after which Mute would mute the entire system.
// This thread does nothing but run the hook, so neither can happen, and the
// render thread is free to release and rebuild its GPU resources while idle.
void InputHook::HookThreadMain(std::promise<bool> installed) {
    m_muteHdrHotkey = {};
    // It runs for microseconds per key press; being scheduled promptly under
    // a full CPU (a game) is what keeps typing latency unchanged.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    // Create the message queue before Install() returns, so Uninstall() can
    // never post WM_QUIT to a thread that does not have one yet.
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    m_hookThreadId = GetCurrentThreadId();

    m_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(nullptr), 0);
    const bool ok = (m_hook != nullptr);
    installed.set_value(ok);
    if (!ok) return;

    // Same 5 s period as ever (invariant #1: never weaken the watchdog); the
    // small tolerance only lets Windows align this wake with others.
    const UINT_PTR watchdog = SetCoalescableTimer(nullptr, 0, kHookWatchdogMs, nullptr, 250);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.wParam == watchdog) {
            PERF_COUNT(hookTimerWakes);
            ReinstallHook();
        }
    }
    KillTimer(nullptr, watchdog);

    UnhookWindowsHookEx(m_hook);
    m_hook = nullptr;
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
    PERF_HOOK_SCOPE();
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* pKbd = (KBDLLHOOKSTRUCT*)lParam;
        if ((pKbd->flags & LLKHF_INJECTED) && pKbd->dwExtraInfo == kHotkeyReplayTag) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }
        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        auto& hook = InputHook::Get();
        const uint32_t hdrBindings = hook.m_hdrHotkeys.load(std::memory_order_relaxed);
        const auto result = hook.m_muteHdrHotkey.Process(pKbd->vkCode, isKeyDown,
            hook.isCapturingKey.load(std::memory_order_acquire),
            HdrHotkey::Unpack(hdrBindings), HdrHotkey::Unpack(hdrBindings >> 16));
        const auto action = result.action;

        if (action == MuteHdrHotkey::Action::BeginMute) {
            // Snapshot the route now, since its modifier may be released before
            // Mute. The action itself waits until we know this was not HDR.
            int matchedBinding = -1;
            for (int bi = 0; bi < (int)g_Config.appBindings.size(); ++bi) {
                if (g_Config.appBindings[bi].modifierKey != 0 &&
                    (GetAsyncKeyState(g_Config.appBindings[bi].modifierKey) & 0x8000)) {
                    matchedBinding = bi;
                    break;
                }
            }
            const bool secondary = (matchedBinding < 0) && SecondaryGroupActive();
            hook.m_pendingMuteEvent = {matchedBinding >= 0 ? 4 : 1,
                                      false, false, matchedBinding, 0, secondary};
            return 1;
        }
        if (action == MuteHdrHotkey::Action::ReplayKey) {
            SendHotkeyReplay(result.replayKey, result.forwardKey, result.forwardDown);
            return 1;
        }
        if (result.maskMenu) {
            // VK 0xE8 is unassigned. A neutral key while Win is down prevents
            // Start opening when Win is released after our swallowed chord.
            SendHotkeyReplay(0xE8);
        }
        if (action == MuteHdrHotkey::Action::PlayPause || action == MuteHdrHotkey::Action::ToggleHdr ||
            action == MuteHdrHotkey::Action::ToggleHdrEffect) {
            std::lock_guard<std::mutex> lock(hook.eventMutex);
            const int type = action == MuteHdrHotkey::Action::ToggleHdrEffect ? 9 : 8;
            hook.pendingEvents.push_back(action == MuteHdrHotkey::Action::PlayPause
                ? hook.m_pendingMuteEvent : HookEvent{type, false, false});
            hook.eventCv.notify_one();
            return 1;
        }
        if (action == MuteHdrHotkey::Action::Consume) return 1;

        if (isKeyDown) {

            // Passed straight through - this key belongs to whatever app has
            // focus - but its press is remembered, because a keyboard that
            // auto-releases Pause/Break makes "is it held" unanswerable later.
            if (g_Config.secondaryKey != 0 && pKbd->vkCode == (DWORD)g_Config.secondaryKey) {
                g_secondaryDownTick.store(GetTickCount64(), std::memory_order_relaxed);
            }

            if (hook.isCapturingKey.load(std::memory_order_acquire)) {
                int vk = pKbd->vkCode;
                const int captureType = hook.captureType.load(std::memory_order_relaxed);
                if ((captureType == 7 || captureType == 8) && !HdrHotkey::IsPrimaryKey(vk)) {
                    return CallNextHookEx(hook.m_hook, nCode, wParam, lParam);
                }
                if (vk == VK_VOLUME_UP || vk == VK_VOLUME_DOWN || vk == VK_VOLUME_MUTE ||
                    vk == VK_MEDIA_PLAY_PAUSE || vk == VK_MEDIA_NEXT_TRACK || vk == VK_MEDIA_PREV_TRACK ||
                    vk == VK_MEDIA_STOP) {
                    return CallNextHookEx(InputHook::Get().m_hook, nCode, wParam, lParam);
                }
                // Types 0, 3 and 6 bind a held modifier, so Ctrl/Shift/Alt are
                // legal answers there; every other capture wants a real key.
                if (captureType != 0 && captureType != 3 && captureType != 6) {
                    if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                        vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                        vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU) {
                        return CallNextHookEx(InputHook::Get().m_hook, nCode, wParam, lParam);
                    }
                }
                hook.isCapturingKey.store(false, std::memory_order_relaxed);
                if (captureType == 7 || captureType == 8) {
                    hook.m_muteHdrHotkey.SuppressUntilRelease(vk);
                    if ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN) |
                         GetAsyncKeyState(VK_LMENU)) & 0x8000) SendHotkeyReplay(0xE8);
                }
                if (hook.OnKeyCaptured) hook.OnKeyCaptured(vk, captureType);
                return 1;
            }

            if (pKbd->vkCode == (DWORD)g_Config.menuKey) {
                if (ModifiersHeld(g_Config.menuCtrl, g_Config.menuShift, g_Config.menuAlt)) {
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({2, false, false});
                    hook.eventCv.notify_one();
                    return 1;
                }
            }

            if (g_Config.infoKey != 0 && pKbd->vkCode == (DWORD)g_Config.infoKey) {
                if (ModifiersHeld(g_Config.infoCtrl, g_Config.infoShift, g_Config.infoAlt)) {
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({3, false, false});
                    hook.eventCv.notify_one();
                    return 1;
                }
            }

            // Sample-rate toggle (default Left Alt+Q) and console/monitor swap
            // (default Left Alt+S). Both only enqueue — the worker does the COM,
            // DDC/CI and display work.
            if (g_Config.srKey != 0 && pKbd->vkCode == (DWORD)g_Config.srKey) {
                if (ModifiersHeld(g_Config.srCtrl, g_Config.srShift, g_Config.srAlt)) {
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({5, false, false, -1, 0});
                    hook.eventCv.notify_one();
                    return 1;
                }
            }

            if (g_Config.swapKey != 0 && pKbd->vkCode == (DWORD)g_Config.swapKey) {
                if (ModifiersHeld(g_Config.swapCtrl, g_Config.swapShift, g_Config.swapAlt)) {
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({6, false, false, -1, 0});
                    hook.eventCv.notify_one();
                    return 1;
                }
            }

            if (pKbd->vkCode == VK_VOLUME_UP || pKbd->vkCode == VK_VOLUME_DOWN) {
                // Check per-app bindings first (higher priority than global modifier)
                int matchedBinding = -1;
                for (int bi = 0; bi < (int)g_Config.appBindings.size(); ++bi) {
                    if (g_Config.appBindings[bi].modifierKey != 0 &&
                        (GetAsyncKeyState(g_Config.appBindings[bi].modifierKey) & 0x8000)) {
                        matchedBinding = bi;
                        break;
                    }
                }

                if (matchedBinding >= 0) {
                    bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    bool up = (pKbd->vkCode == VK_VOLUME_UP);
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({0, up, isShift, matchedBinding});
                    hook.eventCv.notify_one();
                    return 1;
                }

                // Fall through to the normal modifier gate
                bool modifierPressed = false;
                if (g_Config.modifierKey == 0) {
                    modifierPressed = true;
                } else {
                    modifierPressed = (GetAsyncKeyState(g_Config.modifierKey) & 0x8000) != 0;
                }

                // Holding the browser-group key is intent enough on its own,
                // so it also satisfies a configured global volume modifier.
                const bool secondary = SecondaryGroupActive();

                if (modifierPressed || secondary) {
                    bool isShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    bool up = (pKbd->vkCode == VK_VOLUME_UP);
                    std::lock_guard<std::mutex> lock(hook.eventMutex);
                    hook.pendingEvents.push_back({0, up, isShift, -1, 0, secondary});
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

    // Hoisted out of the loop and swapped rather than copied, so a burst of
    // roller ticks reuses one buffer instead of allocating a fresh vector (and
    // freeing the queue's) on every wake-up.
    std::vector<HookEvent> events;

    while (!m_stopWorker) {
        events.clear();
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
            // The sample-rate auto-revert / reminder rides on the same wait, so
            // no extra thread is needed. A deadline hours out simply wakes us on
            // the hourly default and gets recomputed.
            const bool quickTimerPending = QuickActions::Get().HasTimer();
            if (quickTimerPending) {
                deadline = (std::min)(deadline, QuickActions::Get().NextDeadline());
            }

            if (debounceActive || isStreaming || quickTimerPending) {
                eventCv.wait_until(lock, deadline, [this]() {
                    return m_stopWorker || !pendingEvents.empty();
                });
            } else {
                eventCv.wait(lock, [this]() { return m_stopWorker || !pendingEvents.empty(); });
            }

            if (m_stopWorker) break;
            events.swap(pendingEvents);
        }
        PERF_COUNT(workerWakes);
#ifdef SVM_PERF
        for (const HookEvent& pev : events) {
            LONGLONG zero = 0;
            if (pev.type == 0) Perf::osdSeriesStart.compare_exchange_strong(zero, Perf::Now());
            else if (pev.type == 3) Perf::trackStart.compare_exchange_strong(zero, Perf::Now());
        }
#endif

        auto now = std::chrono::steady_clock::now();

        // Process timeouts first
        if (isStreaming && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStreamTick).count() >= streamTimeoutMs) {
            isStreaming = false;
        }
        if (debounceActive && std::chrono::duration_cast<std::chrono::milliseconds>(now - debounceStart).count() >= g_Config.volDebounceWindowMs) {
            debounceActive = false;
            accumulatedTicks = 0;
            PERF_ONLY(Perf::osdSeriesStart.store(0);) // the series never fired
        }

        // Volume, track info and the quick actions (notice popup) can all end
        // in something on screen; play/pause (1, 4) and the menu toggle (2,
        // drawn on the very next frame regardless) do not need the head start.
        if (OnPopupLikely) {
            for (const HookEvent& pev : events) {
                if (pev.type == 0 || pev.type == 3 || pev.type == 5 || pev.type == 6 ||
                    pev.type == 8 || pev.type == 9) {
                    OnPopupLikely();
                    break;
                }
            }
        }

        for (size_t i = 0; i < events.size(); ++i) {
            const HookEvent& ev = events[i];
            if (ev.type == 1) {
                SafeTogglePlayPause(ev.secondaryGroup);
            } else if (ev.type == 4) {
                // Per-app play/pause toggle via SMTC
                if (ev.bindingIndex >= 0 && ev.bindingIndex < (int)g_Config.appBindings.size()) {
                    const auto& binding = g_Config.appBindings[ev.bindingIndex];
                    SafeTogglePlayPauseForApp(binding.targetApp.c_str());
                }
            } else if (ev.type == 2) {
                if (OnMenuToggled) OnMenuToggled();
            } else if (ev.type == 3) {
                std::string track;
                if (SafeGetTrackInfo(&track) && OnTrackInfoRequested) OnTrackInfoRequested(track);
            } else if (ev.type == 5) {
                SafeSampleRateAction(ev.actionArg);
            } else if (ev.type == 6) {
                SafeDisplayAction(ev.actionArg);
            } else if (ev.type == 7) {
                SafeRefreshMonitors();
            } else if (ev.type == 8) {
                SafeHdrAction();
            } else if (ev.type == 9) {
                SafeMonitorHdrEffectAction();
            } else if (ev.type == 0) {
                // A noisy roller nudge often lands several identical ticks in a
                // single wake-up. They are folded into one run here and applied
                // as one multi-step change: one session enumeration and one OSD
                // update instead of one of each per tick. The debounce
                // accumulator sees exactly the same tick total either way.
                int run = 1;
                while (i + run < events.size() && SameVolumeTick(events[i + run], ev)) ++run;
                i += run - 1;

                // Determine if this is per-app or global volume
                bool isPerApp = (ev.bindingIndex >= 0 && ev.bindingIndex < (int)g_Config.appBindings.size());
                std::string perAppTarget;
                if (isPerApp) {
                    perAppTarget = g_Config.appBindings[ev.bindingIndex].targetApp;
                }

                auto apply = [&](bool up, bool largeStep, int steps) {
                    float newVol = 0.0f;
                    PERF_ONLY(LONGLONG zero = 0; Perf::osdFire.compare_exchange_strong(zero, Perf::Now());)
                    bool changed = false;
                    if (isPerApp) {
                        if (SafeChangeVolumeForApp(perAppTarget.c_str(), up, largeStep, newVol, steps)) {
                            changed = true;
                            if (OnVolumeChanged) OnVolumeChanged(newVol, perAppTarget);
                        }
                    } else {
                        std::string appName;
                        if (SafeChangeVolume(up, largeStep, newVol, &appName, ev.secondaryGroup, steps)) {
                            changed = true;
                            if (OnVolumeChanged) OnVolumeChanged(newVol, appName);
                        }
                    }
                    PERF_ONLY(if (!changed) { Perf::osdSeriesStart.store(0); Perf::osdFire.store(0); })
                    (void)changed;
                };

                if (ev.volLargeStep) {
                    isStreaming = true;
                    debounceActive = false;
                    lastStreamTick = now;
                    accumulatedTicks = 0;
                    apply(ev.volUp, true, run);
                } else if (isStreaming) {
                    lastStreamTick = now;
                    apply(ev.volUp, false, run);
                } else {
                    if (!debounceActive) {
                        // Start debounce
                        debounceActive = true;
                        debounceStart = now;
                        accumulatedTicks = 0;
                    }
                    accumulatedTicks += ev.volUp ? run : -run;

                    // Threshold 1 is the "no debounce" fast path: the very first
                    // tick fires immediately.
                    const int threshold = (std::max)(1, g_Config.volDebounceThreshold);
                    if (std::abs(accumulatedTicks) >= threshold) {
                        apply(accumulatedTicks > 0, false, std::abs(accumulatedTicks));
                        isStreaming = true;
                        debounceActive = false;
                        lastStreamTick = now;
                        accumulatedTicks = 0;
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

        SafeProcessTimers();
    }

    // Last chance to hand the DAC its normal sample rate back. This runs before
    // CoUninitialize so the restore still has a live MTA apartment, and it is
    // the reason Uninstall() joins this thread instead of detaching it.
    SafeQuickActionsShutdown();
    SafeReleaseAudioThreadCache();

    CoUninitialize();
}
