#include "AudioManager.h"
#include "Config.h"
#include "StrUtil.h"
#include "Perf.h"
#include <iostream>
#include <algorithm>
#include <atomic>
#include <string.h>
#include <unordered_map>
#include <tlhelp32.h>
#include <psapi.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "windowsapp.lib")

using namespace Microsoft::WRL;
using namespace winrt::Windows::Media::Control;

namespace {

// --- Per-thread WASAPI session-chain cache ---------------------------------
// Reaching the session list costs four steps, three of which are round trips
// into the audio service: CoCreateInstance(MMDeviceEnumerator) ->
// GetDefaultAudioEndpoint -> IMMDevice::Activate(IAudioSessionManager2) ->
// GetSessionEnumerator. The volume roller fires bursts of ticks and each tick
// paid for all four, which made this the single most expensive thing the app
// does.
//
// The device enumerator and the session manager are long-lived objects, so they
// are kept; only the session enumerator — a *snapshot* of the session list — is
// still taken fresh on every call, so an app that starts playing is picked up
// exactly as before.
//
// The cache is thread_local, and that is what preserves the apartment rule the
// original per-call construction existed for: a pointer built on the STA main
// thread can never be reached from the MTA worker, because each thread only
// ever sees its own copy. It is released explicitly (AudioManager::
// ReleaseThreadCache) before each thread's CoUninitialize; a thread_local
// destructor would run too late, after COM is already gone.
constexpr ULONGLONG kChainTtlMs = 1500;

struct SessionChainCache {
    ComPtr<IMMDeviceEnumerator> devEnum;
    ComPtr<IAudioSessionManager2> manager;
    ULONGLONG builtAtMs = 0;
    unsigned generation = 0;
    std::unordered_map<DWORD, std::string> procNames; // pid -> lower-case basename

    void Reset() {
        manager.Reset();
        devEnum.Reset();
        builtAtMs = 0;
        procNames.clear();
    }
};

thread_local SessionChainCache t_chain;

// Bumped whenever the default output endpoint changes, so a cached chain that
// still points at the old device is dropped at once instead of lingering out
// the rest of its TTL.
std::atomic<unsigned> g_deviceGeneration{1};

// --- Per-thread SMTC manager cache -----------------------------------------
// GlobalSystemMediaTransportControlsSessionManager::RequestAsync() is an
// asynchronous activation of an out-of-process broker, and every Mute press
// and every track-info request used to pay for it from scratch. The manager is
// designed to be long-lived (GetSessions() always returns a fresh snapshot), so
// it is requested once per thread and kept. thread_local for the same reason
// as the WASAPI chain above, and released by the same ReleaseThreadCache()
// call before the thread's CoUninitialize. A broker that has gone away
// (explorer restart) surfaces as a throw from GetSessions(), which drops the
// cached manager and retries once with a freshly requested one.
thread_local GlobalSystemMediaTransportControlsSessionManager t_smtcManager{ nullptr };

// One snapshot of the SMTC session list. GetMany pulls the whole vector in a
// single call instead of a Size()/GetAt() round trip per element.
std::vector<GlobalSystemMediaTransportControlsSession> SnapshotSmtcSessions() {
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            if (!t_smtcManager) t_smtcManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            if (!t_smtcManager) return {};

            auto view = t_smtcManager.GetSessions();
            if (!view) return {};

            // resize() takes the fill value explicitly: projected WinRT types
            // have no default constructor, only one from nullptr.
            std::vector<GlobalSystemMediaTransportControlsSession> sessions(view.Size(), nullptr);
            if (!sessions.empty()) sessions.resize(view.GetMany(0, sessions), nullptr);
            return sessions;
        } catch (...) {
            t_smtcManager = nullptr;
        }
    }
    return {};
}

// UTF-8 AUMID of a session, or empty when the session has gone away.
std::string SessionAumid(const GlobalSystemMediaTransportControlsSession& session) {
    try {
        return ToUtf8(session.SourceAppUserModelId());
    } catch (...) {
        return {};
    }
}

bool SessionIsPlaying(const GlobalSystemMediaTransportControlsSession& session) {
    try {
        auto playbackInfo = session.GetPlaybackInfo();
        return playbackInfo && playbackInfo.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    } catch (...) {
        return false;
    }
}

} // namespace

AudioManager& AudioManager::Get() {
    static AudioManager instance;
    return instance;
}

bool AudioManager::Initialize() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&m_pEnumerator));
    return SUCCEEDED(hr);
}

void AudioManager::Shutdown() {
    m_pEnumerator.Reset();
    ReleaseThreadCache();
    CoUninitialize();
}

void AudioManager::ReleaseThreadCache() {
    t_chain.Reset();
    t_smtcManager = nullptr;
}

void AudioManager::InvalidateDeviceCache() {
    g_deviceGeneration.fetch_add(1, std::memory_order_relaxed);
}

bool AudioManager::GetSessionEnumerator(ComPtr<IAudioSessionEnumerator>& outSessEnum) {
    // The enumerator/device/manager chain is built in — and only ever touched
    // from — the calling thread, via the thread_local cache above. That keeps
    // the apartment rule this function was originally written for: an STA
    // pointer is never dereferenced from the MTA worker, which used to fault
    // intermittently (killing the process, and with it the mute key) or fail
    // silently (so the OSD never showed).
    //
    // Two attempts: the second one rebuilds from scratch if a cached manager
    // has gone stale (endpoint unplugged, audio service restarted), which is
    // the self-healing property the old per-call construction had for free.
    for (int attempt = 0; attempt < 2; ++attempt) {
        const ULONGLONG now = GetTickCount64();
        const unsigned generation = g_deviceGeneration.load(std::memory_order_relaxed);

        if (t_chain.manager &&
            (t_chain.generation != generation || now - t_chain.builtAtMs > kChainTtlMs)) {
            t_chain.Reset();
        }

        PERF_ONLY(const bool perfCold = !t_chain.manager; const LONGLONG perfT0 = Perf::Now();)
        if (!t_chain.manager) {
            ComPtr<IMMDeviceEnumerator> pEnumerator;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&pEnumerator))) || !pEnumerator)
                return false;

            ComPtr<IMMDevice> pDevice;
            if (FAILED(pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice)) || !pDevice)
                return false;

            ComPtr<IAudioSessionManager2> pManager;
            if (FAILED(pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                         (void**)&pManager)) || !pManager)
                return false;

            t_chain.devEnum = pEnumerator;
            t_chain.manager = pManager;
            t_chain.builtAtMs = now;
            t_chain.generation = generation;
            t_chain.procNames.clear();
        }

        outSessEnum.Reset();
        if (SUCCEEDED(t_chain.manager->GetSessionEnumerator(&outSessEnum)) && outSessEnum) {
            PERF_ONLY(Perf::Record(perfCold ? "audio.GetSessionEnumerator.cold" : "audio.GetSessionEnumerator.warm",
                                   Perf::ToMs(Perf::Now() - perfT0));)
            return true;
        }

        t_chain.Reset();
    }
    return false;
}

const std::string& AudioManager::ProcessNameLower(DWORD pid) {
    auto it = t_chain.procNames.find(pid);
    if (it != t_chain.procNames.end()) return it->second;

    std::string name = Get().GetProcessName(pid);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return t_chain.procNames.emplace(pid, std::move(name)).first->second;
}

std::string AudioManager::GetProcessName(DWORD pid) {
    std::string name = "(PID " + std::to_string(pid) + ")";
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProcess) {
        WCHAR szProcessName[MAX_PATH];
        DWORD size = sizeof(szProcessName)/sizeof(WCHAR);
        if (QueryFullProcessImageNameW(hProcess, 0, szProcessName, &size)) {
            std::wstring fullPath(szProcessName);
            size_t pos = fullPath.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                fullPath = fullPath.substr(pos + 1);
            }
            char buffer[MAX_PATH];
            WideCharToMultiByte(CP_UTF8, 0, fullPath.c_str(), -1, buffer, MAX_PATH, NULL, NULL);
            name = buffer;
        }
        CloseHandle(hProcess);
    }
    return name;
}

// steps > 1 applies a whole debounce burst in one pass over the sessions; the
// clamp makes that identical to applying it one tick at a time.
static float SteppedVolume(float current, bool up, bool largeStep, int steps) {
    const float step = (largeStep ? g_Config.volStepLarge : g_Config.volStep) * (float)(steps < 1 ? 1 : steps);
    float newVol = up ? (current + step) : (current - step);
    if (newVol > 1.0f) newVol = 1.0f;
    if (newVol < 0.0f) newVol = 0.0f;
    return newVol;
}

bool AudioManager::ChangeVolume(bool up, bool largeStep, float& outLastVol, std::string& outAppName,
                                bool secondaryGroup, int steps) {
    PERF_SCOPE("audio.ChangeVolume");
    ComPtr<IAudioSessionEnumerator> pSessEnum;
    if (!GetSessionEnumerator(pSessEnum)) return false;

    int count = 0;
    pSessEnum->GetCount(&count);

    bool found = false;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> pControl;
        if (FAILED(pSessEnum->GetSession(i, &pControl))) continue;

        ComPtr<IAudioSessionControl2> pControl2;
        if (FAILED(pControl.As(&pControl2))) continue;

        DWORD pid = 0;
        pControl2->GetProcessId(&pid);

        const std::string& procName = ProcessNameLower(pid);

        if (!ProcessInGroup(procName, secondaryGroup)) continue;

        ComPtr<ISimpleAudioVolume> pVolume;
        if (FAILED(pControl2.As(&pVolume))) continue;

        float currentVol = 0.0f;
        pVolume->GetMasterVolume(&currentVol);

        const float newVol = SteppedVolume(currentVol, up, largeStep, steps);

        pVolume->SetMasterVolume(newVol, nullptr);
        outLastVol = newVol;
        outAppName = procName;
        found = true;
    }

    return found;
}

bool AudioManager::ChangeVolumeForApp(const std::string& appName, bool up, bool largeStep, float& outVol,
                                      int steps) {
    ComPtr<IAudioSessionEnumerator> pSessEnum;
    if (!GetSessionEnumerator(pSessEnum)) return false;

    int count = 0;
    pSessEnum->GetCount(&count);

    bool found = false;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> pControl;
        if (FAILED(pSessEnum->GetSession(i, &pControl))) continue;

        ComPtr<IAudioSessionControl2> pControl2;
        if (FAILED(pControl.As(&pControl2))) continue;

        DWORD pid = 0;
        pControl2->GetProcessId(&pid);

        if (_stricmp(ProcessNameLower(pid).c_str(), appName.c_str()) != 0) continue;

        ComPtr<ISimpleAudioVolume> pVolume;
        if (FAILED(pControl2.As(&pVolume))) continue;

        float currentVol = 0.0f;
        pVolume->GetMasterVolume(&currentVol);

        const float newVol = SteppedVolume(currentVol, up, largeStep, steps);

        pVolume->SetMasterVolume(newVol, nullptr);
        outVol = newVol;
        found = true;
    }

    return found;
}

bool AudioManager::ToggleMuteForApp(const std::string& appName, bool& outMuted, float& outVol) {
    ComPtr<IAudioSessionEnumerator> pSessEnum;
    if (!GetSessionEnumerator(pSessEnum)) return false;

    int count = 0;
    pSessEnum->GetCount(&count);

    bool found = false;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> pControl;
        if (FAILED(pSessEnum->GetSession(i, &pControl))) continue;

        ComPtr<IAudioSessionControl2> pControl2;
        if (FAILED(pControl.As(&pControl2))) continue;

        DWORD pid = 0;
        pControl2->GetProcessId(&pid);

        if (_stricmp(ProcessNameLower(pid).c_str(), appName.c_str()) != 0) continue;

        ComPtr<ISimpleAudioVolume> pVolume;
        if (FAILED(pControl2.As(&pVolume))) continue;

        BOOL currentMute = FALSE;
        pVolume->GetMute(&currentMute);

        BOOL newMute = currentMute ? FALSE : TRUE;
        pVolume->SetMute(newMute, nullptr);

        float vol = 0.0f;
        pVolume->GetMasterVolume(&vol);

        outMuted = (newMute != FALSE);
        outVol = vol;
        found = true;
    }

    return found;
}

std::vector<AudioSessionInfo> AudioManager::GetActiveSessions() {
    std::vector<AudioSessionInfo> sessions;
    ComPtr<IAudioSessionEnumerator> pSessEnum;
    if (!GetSessionEnumerator(pSessEnum)) return sessions;

    int count = 0;
    pSessEnum->GetCount(&count);

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> pControl;
        if (FAILED(pSessEnum->GetSession(i, &pControl))) continue;

        ComPtr<IAudioSessionControl2> pControl2;
        if (FAILED(pControl.As(&pControl2))) continue;

        DWORD pid = 0;
        pControl2->GetProcessId(&pid);
        
        ComPtr<ISimpleAudioVolume> pVolume;
        float vol = 0.0f;
        BOOL muted = FALSE;
        if (SUCCEEDED(pControl2.As(&pVolume))) {
            pVolume->GetMasterVolume(&vol);
            pVolume->GetMute(&muted);
        }

        AudioSessionInfo info;
        info.processName = GetProcessName(pid);
        info.pid = pid;
        info.volume = vol;
        info.muted = (muted != FALSE);
        sessions.push_back(info);
    }

    return sessions;
}

bool AudioManager::NameMatchesExact(const std::string& procName, const std::vector<std::string>& list) {
    for (const auto& app : list) {
        // _stricmp instead of building a lower-case copy of every configured
        // app name for every session: this ran sessions x apps times per tick.
        if (_stricmp(procName.c_str(), app.c_str()) == 0) return true;
    }
    return false;
}

bool AudioManager::AumidMatches(const std::string& sourceAppId, const std::vector<std::string>& list) {
    if (sourceAppId.empty()) return false;

    // Case-insensitive compares over the originals rather than building a
    // lower-case copy of the AUMID and of every configured app name.
    for (const auto& app : list) {
        // Direct match (e.g. "spotify.exe" == "spotify.exe")
        if (_stricmp(sourceAppId.c_str(), app.c_str()) == 0) return true;

        // Or the AUMID merely contains the app name with ".exe" trimmed off
        // (e.g. "Spotify.exe" matches an AUMID containing "spotify").
        std::string_view base(app);
        const size_t dotPos = base.rfind('.');
        if (dotPos != std::string_view::npos) base = base.substr(0, dotPos);
        if (!base.empty() && ContainsNoCase(sourceAppId, base)) return true;
    }
    return false;
}

// The browser group wins on purpose. Without this, an app listed in both groups
// - or, worse, a browser whose AUMID happens to contain an audio app's name -
// would still be reachable from a bare roller tick or Mute press, which is the
// whole behaviour this split exists to stop.
bool AudioManager::ProcessInGroup(const std::string& procName, bool secondaryGroup) {
    if (secondaryGroup) return NameMatchesExact(procName, g_Config.secondaryApps);
    if (NameMatchesExact(procName, g_Config.secondaryApps)) return false;
    return NameMatchesExact(procName, g_Config.mediaApps);
}

bool AudioManager::SessionInGroup(const std::string& sourceAppId, bool secondaryGroup) {
    if (secondaryGroup) return AumidMatches(sourceAppId, g_Config.secondaryApps);
    if (AumidMatches(sourceAppId, g_Config.secondaryApps)) return false;
    return AumidMatches(sourceAppId, g_Config.mediaApps);
}

bool AudioManager::TogglePlayPause(bool secondaryGroup) {
    PERF_SCOPE("audio.TogglePlayPause");
    try {
        // Find the best matching session: prefer a currently playing one
        GlobalSystemMediaTransportControlsSession targetSession = nullptr;

        for (const auto& session : SnapshotSmtcSessions()) {
            if (!session) continue;

            const std::string sourceId = SessionAumid(session);
            if (!SessionInGroup(sourceId, secondaryGroup)) continue;

            if (!targetSession) {
                targetSession = session; // First matching configured session
            }

            if (SessionIsPlaying(session)) {
                targetSession = session; // Prefer the one currently playing
                break;
            }
        }

        if (targetSession) {
            try {
                targetSession.TryTogglePlayPauseAsync().get();
            } catch (...) {}
            return true;
        }
    } catch (winrt::hresult_error const&) {
    } catch (std::exception const&) {
    } catch (...) {}
    return false;
}

bool AudioManager::TogglePlayPauseForApp(const std::string& appName) {
    try {
        // Strip .exe for loose AUMID matching
        std::string_view targetBase(appName);
        const size_t dotPos = targetBase.rfind('.');
        if (dotPos != std::string_view::npos) {
            targetBase = targetBase.substr(0, dotPos);
        }

        GlobalSystemMediaTransportControlsSession targetSession = nullptr;

        for (const auto& session : SnapshotSmtcSessions()) {
            if (!session) continue;

            const std::string sourceId = SessionAumid(session);
            if (sourceId.empty()) continue;

            // Exact match or AUMID contains the app base name
            const bool match = _stricmp(sourceId.c_str(), appName.c_str()) == 0 ||
                               (!targetBase.empty() && ContainsNoCase(sourceId, targetBase));
            if (!match) continue;

            if (!targetSession) {
                targetSession = session;
            }

            if (SessionIsPlaying(session)) {
                targetSession = session;
                break;
            }
        }

        if (targetSession) {
            try {
                targetSession.TryTogglePlayPauseAsync().get();
            } catch (...) {}
            return true;
        }
    } catch (winrt::hresult_error const&) {
    } catch (std::exception const&) {
    } catch (...) {}
    return false;
}

std::string AudioManager::GetCurrentTrackInfo() {
    PERF_SCOPE("audio.GetCurrentTrackInfo");
    try {
        // Each session's AUMID and playing state are read once up front (both
        // are calls into another process) instead of once per pass below.
        struct Candidate {
            GlobalSystemMediaTransportControlsSession session;
            std::string sourceId;
        };
        std::vector<Candidate> playing;
        for (const auto& session : SnapshotSmtcSessions()) {
            if (!session) continue;
            std::string sourceId = SessionAumid(session);
            if (sourceId.empty()) continue;
            if (!SessionIsPlaying(session)) continue; // Skip non-playing sessions
            playing.push_back({ session, std::move(sourceId) });
        }

        // Audio apps first, browsers only if none of them is playing. Reading a
        // title changes nothing, so unlike the roller and the Mute key this one
        // still reports a browser tab without the group key being held - it just
        // never shadows a real player.
        for (int pass = 0; pass < 2; ++pass) {
            const bool secondaryGroup = (pass == 1);

            for (const auto& candidate : playing) {
                if (!SessionInGroup(candidate.sourceId, secondaryGroup)) continue;

                try {
                    auto info = candidate.session.TryGetMediaPropertiesAsync().get();
                    if (!info) continue;

                    const std::string utf8Title = ToUtf8(info.Title());
                    const std::string utf8Artist = ToUtf8(info.Artist());

                    if (!utf8Title.empty() || !utf8Artist.empty()) {
                        if (utf8Artist.empty()) return utf8Title;
                        return utf8Artist + " - " + utf8Title;
                    }
                } catch (...) { continue; }
            }
        }
    } catch (winrt::hresult_error const&) {
    } catch (std::exception const&) {
    } catch (...) {}
    return "";
}
