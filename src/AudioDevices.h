#pragma once
#include <string>
#include <vector>

struct RenderDeviceInfo {
    std::wstring id;        // MMDevice endpoint id (the key every API below takes)
    std::string name;       // UTF-8 friendly name, e.g. "Speakers (FiiO K7)"
    bool isDefault = false;
    int sampleRate = 0;     // current "Default Format" rate in Hz, 0 = unknown
    int bitDepth = 0;
    int channels = 0;
};

// Endpoint-level output device control: the sample rate behind the Sound
// control panel's "Default Format" dropdown, plus the default-device switch.
//
// Every function creates its COM objects locally, so it is apartment-safe by
// construction (see the AudioManager::GetSessionEnumerator comment for why that
// matters here). Mutating calls are only ever issued from the MTA worker
// thread; the read-only ones are also used by the settings menu on the UI
// thread.
namespace AudioDevices {

std::vector<RenderDeviceInfo> ListRenderDevices();

// Locates a render endpoint by a case-insensitive substring of its friendly
// name. An empty match resolves to the current default output device.
bool ResolveDevice(const std::string& nameMatch, RenderDeviceInfo& out);

bool GetSampleRate(const std::wstring& deviceId, int& outHz);

// Rewrites only the sample rate of the endpoint's default format, keeping its
// channel count, bit depth and channel mask. outError receives a short reason
// on failure (or, if the live path failed but the fallback still wrote the
// stored default, the reason the live path was skipped).
//
// outAppliedLive distinguishes the two success paths, because they behave
// differently in practice: IPolicyConfig::SetDeviceFormat is the same call the
// Sound control panel's "Default Format" dropdown makes, so it reconfigures the
// live audio engine immediately (outAppliedLive=true). The property-store
// fallback only rewrites the stored default for the *next* time the engine
// opens the endpoint — on this Windows build that has NOT been observed to
// take effect until the endpoint is reopened (e.g. replug the DAC or reboot),
// so a caller should tell the user the change may not be audible yet.
bool SetSampleRate(const std::wstring& deviceId, int hz, std::string* outError, bool* outAppliedLive = nullptr);

bool GetDefaultRenderDeviceId(std::wstring& outId);
bool SetDefaultRenderDevice(const std::wstring& deviceId, std::string* outError);

} // namespace AudioDevices
