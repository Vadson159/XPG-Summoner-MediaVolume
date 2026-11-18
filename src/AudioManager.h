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
    // secondaryGroup picks Config::secondaryApps (the browsers) instead of
    // Config::mediaApps - the two groups are never touched by the same tick.
    // steps applies that many ticks in a single pass over the sessions (the
    // debounce burst), instead of one full session enumeration per tick.
    bool ChangeVolume(bool up, bool largeStep, float& outLastVol, std::string& outAppName,
                      bool secondaryGroup = false, int steps = 1);

    // Change volume for a single app by process name. Returns true if found.
    bool ChangeVolumeForApp(const std::string& appName, bool up, bool largeStep, float& outVol,
                            int steps = 1);

    // Toggle mute for a single app by process name. Returns true if found.
    // outMuted = new mute state, outVol = current volume level.
    bool ToggleMuteForApp(const std::string& appName, bool& outMuted, float& outVol);

    // Get list of active audio sessions for the UI
    std::vector<AudioSessionInfo> GetActiveSessions();

    // Get current track info from SMTC (only from configured apps)
    std::string GetCurrentTrackInfo();

    // Toggle play/pause only for configured apps via SMTC. secondaryGroup picks
    // the browser whitelist instead of the audio-app one.
    bool TogglePlayPause(bool secondaryGroup = false);

    // Toggle play/pause for a single app by process name via SMTC
    bool TogglePlayPauseForApp(const std::string& appName);

    // Drops the per-thread WASAPI cache built by GetSessionEnumerator and the
    // cached SMTC session manager. MUST be called on a thread before that
    // thread calls CoUninitialize — the cached interface pointers belong to its
    // apartment, and releasing them after COM has been torn down is undefined.
    static void ReleaseThreadCache();

    // Marks every thread's cached chain stale. Called when the default output
    // endpoint is switched out from under us, so a volume tick can never keep
    // driving sessions on the device we just left.
    static void InvalidateDeviceCache();

private:
    AudioManager() = default;
    ~AudioManager() = default;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_pEnumerator;

    // Helper to get session enumerator
    bool GetSessionEnumerator(Microsoft::WRL::ComPtr<IAudioSessionEnumerator>& outSessEnum);

    std::string GetProcessName(DWORD pid);

    // Lower-cased process basename for a session's pid, memoised for as long as
    // the cached session chain lives. Resolving one costs an OpenProcess plus a
    // QueryFullProcessImageName, and it used to be paid for every live session
    // on every single roller tick.
    static const std::string& ProcessNameLower(DWORD pid);

    // --- Whitelist group membership ----------------------------------------
    // Volume matches a process basename exactly; SMTC only ever hands out an
    // AUMID, so that one also accepts "the AUMID contains the app name with
    // .exe trimmed". Both resolve the *group* through InGroup(), where the
    // browser list deliberately wins: a browser session must never be
    // reachable from the audio group, however loosely its AUMID matches.
    static bool NameMatchesExact(const std::string& procName, const std::vector<std::string>& list);
    static bool AumidMatches(const std::string& sourceAppId, const std::vector<std::string>& list);
    static bool ProcessInGroup(const std::string& procName, bool secondaryGroup);
    static bool SessionInGroup(const std::string& sourceAppId, bool secondaryGroup);
};
