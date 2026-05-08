#include "AudioManager.h"
#include "Config.h"
#include <iostream>
#include <algorithm>
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
    CoUninitialize();
}

bool AudioManager::GetSessionEnumerator(ComPtr<IAudioSessionEnumerator>& outSessEnum) {
    if (!m_pEnumerator) return false;

    ComPtr<IMMDevice> pDevice;
    HRESULT hr = m_pEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDevice);
    if (FAILED(hr) || !pDevice)
        return false;

    ComPtr<IAudioSessionManager2> pManager;
    hr = pDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&pManager);
    if (FAILED(hr) || !pManager)
        return false;

    hr = pManager->GetSessionEnumerator(&outSessEnum);
    if (FAILED(hr) || !outSessEnum)
        return false;

    return true;
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

bool AudioManager::ChangeVolume(bool up, bool largeStep, float& outLastVol, std::string& outAppName) {
    ComPtr<IAudioSessionEnumerator> pSessEnum;
    if (!GetSessionEnumerator(pSessEnum)) return false;

    int count = 0;
    pSessEnum->GetCount(&count);

    float step = largeStep ? g_Config.volStepLarge : g_Config.volStep;
    bool found = false;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> pControl;
        if (FAILED(pSessEnum->GetSession(i, &pControl))) continue;

        ComPtr<IAudioSessionControl2> pControl2;
        if (FAILED(pControl.As(&pControl2))) continue;

        DWORD pid = 0;
        pControl2->GetProcessId(&pid);
        
        std::string procName = GetProcessName(pid);
        std::transform(procName.begin(), procName.end(), procName.begin(), ::tolower);

        bool isTarget = false;
        for (const auto& app : g_Config.mediaApps) {
            std::string appLower = app;
            std::transform(appLower.begin(), appLower.end(), appLower.begin(), ::tolower);
            if (procName == appLower) {
                isTarget = true;
                break;
            }
        }

        if (!isTarget) continue;

        ComPtr<ISimpleAudioVolume> pVolume;
        if (FAILED(pControl2.As(&pVolume))) continue;

        float currentVol = 0.0f;
        pVolume->GetMasterVolume(&currentVol);

        float newVol = up ? (currentVol + step) : (currentVol - step);
        if (newVol > 1.0f) newVol = 1.0f;
        if (newVol < 0.0f) newVol = 0.0f;

        pVolume->SetMasterVolume(newVol, nullptr);
        outLastVol = newVol;
        outAppName = procName;
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

bool AudioManager::IsSessionFromConfiguredApp(const std::string& sourceAppId) {
    if (sourceAppId.empty()) return false;

    std::string aumidLower = sourceAppId;
    std::transform(aumidLower.begin(), aumidLower.end(), aumidLower.begin(), ::tolower);

    for (const auto& app : g_Config.mediaApps) {
        std::string appLower = app;
        std::transform(appLower.begin(), appLower.end(), appLower.begin(), ::tolower);

        // Direct match (e.g. "spotify.exe" == "spotify.exe")
        if (aumidLower == appLower) return true;

        // Strip .exe from app name for partial matching
        std::string appBase = appLower;
        size_t dotPos = appBase.rfind('.');
        if (dotPos != std::string::npos) {
            appBase = appBase.substr(0, dotPos);
        }

        // Check if AUMID contains the app base name (e.g. "Spotify.exe" contains "spotify")
        if (!appBase.empty() && aumidLower.find(appBase) != std::string::npos) return true;
    }
    return false;
}

bool AudioManager::TogglePlayPause() {
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        if (!manager) return false;
        auto sessions = manager.GetSessions();
        if (!sessions) return false;

        // Find the best matching session: prefer a currently playing one
        GlobalSystemMediaTransportControlsSession targetSession = nullptr;

        for (uint32_t idx = 0; idx < sessions.Size(); ++idx) {
            auto session = sessions.GetAt(idx);
            if (!session) continue;

            std::wstring sourceIdW;
            try {
                sourceIdW = session.SourceAppUserModelId().c_str();
            } catch (...) { continue; }

            char buffer[1024];
            WideCharToMultiByte(CP_UTF8, 0, sourceIdW.c_str(), -1, buffer, sizeof(buffer), NULL, NULL);
            std::string sourceId = buffer;

            if (!IsSessionFromConfiguredApp(sourceId)) continue;

            if (!targetSession) {
                targetSession = session; // First matching configured session
            }

            try {
                auto playbackInfo = session.GetPlaybackInfo();
                if (playbackInfo && playbackInfo.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                    targetSession = session; // Prefer the one currently playing
                    break;
                }
            } catch (...) {}
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
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        if (!manager) return "";
        auto sessions = manager.GetSessions();
        if (!sessions) return "";

        for (uint32_t idx = 0; idx < sessions.Size(); ++idx) {
            auto session = sessions.GetAt(idx);
            if (!session) continue;

            std::wstring sourceIdW;
            try {
                sourceIdW = session.SourceAppUserModelId().c_str();
            } catch (...) { continue; }

            char buffer[1024];
            WideCharToMultiByte(CP_UTF8, 0, sourceIdW.c_str(), -1, buffer, sizeof(buffer), NULL, NULL);
            std::string sourceId = buffer;

            if (!IsSessionFromConfiguredApp(sourceId)) continue;

            try {
                auto playbackInfo = session.GetPlaybackInfo();
                if (playbackInfo && playbackInfo.PlaybackStatus() != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing) {
                    continue; // Skip non-playing sessions
                }
            } catch (...) { continue; }

            try {
                auto info = session.TryGetMediaPropertiesAsync().get();
                if (!info) continue;

                std::wstring title = info.Title().c_str();
                std::wstring artist = info.Artist().c_str();
                
                std::string utf8Title, utf8Artist;
                if (!title.empty()) {
                    char buf[1024];
                    WideCharToMultiByte(CP_UTF8, 0, title.c_str(), -1, buf, sizeof(buf), NULL, NULL);
                    utf8Title = buf;
                }
                if (!artist.empty()) {
                    char buf[1024];
                    WideCharToMultiByte(CP_UTF8, 0, artist.c_str(), -1, buf, sizeof(buf), NULL, NULL);
                    utf8Artist = buf;
                }
                
                if (!utf8Title.empty() || !utf8Artist.empty()) {
                    if (utf8Artist.empty()) return utf8Title;
                    return utf8Artist + " - " + utf8Title;
                }
            } catch (...) { continue; }
        }
    } catch (winrt::hresult_error const&) {
    } catch (std::exception const&) {
    } catch (...) {}
    return "";
}
