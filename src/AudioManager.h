#pragma once
#include <vector>
#include <string>
#include <wrl/client.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

struct AudioSessionInfo {
    std::string processName;
    DWORD pid;
    float volume;
    bool muted;
};

class AudioManager {
public:
    static AudioManager& Get();

    bool Initialize();
    void Shutdown();

    // Change volume for apps in Config. Returns true if at least one was found.
    // 'outLastVol' contains the volume (0.0 to 1.0) of the last found app.
    bool ChangeVolume(bool up, bool largeStep, float& outLastVol, std::string& outAppName);

    // Get list of active audio sessions for the UI
    std::vector<AudioSessionInfo> GetActiveSessions();

    // Get current track info from SMTC (only from configured apps)
    std::string GetCurrentTrackInfo();

    // Toggle play/pause only for configured apps via SMTC
    bool TogglePlayPause();

private:
    AudioManager() = default;
    ~AudioManager() = default;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_pEnumerator;

    // Helper to get session enumerator
    bool GetSessionEnumerator(Microsoft::WRL::ComPtr<IAudioSessionEnumerator>& outSessEnum);

    std::string GetProcessName(DWORD pid);

    // Check if an SMTC session's SourceAppUserModelId matches any configured media app
    bool IsSessionFromConfiguredApp(const std::string& sourceAppId);
};
