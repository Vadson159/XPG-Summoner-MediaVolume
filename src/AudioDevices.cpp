#include "AudioDevices.h"
#include "AudioManager.h"
#include "PolicyConfig.h"
#include "StrUtil.h"

#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <wrl/client.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

// PROPERTYKEYs are spelled out here instead of pulling in
// functiondiscoverykeys_devpkey.h: that header only declares them (the storage
// lives in uuid.lib) and its names would collide with the SDK's own
// definitions. Local constants keep this translation unit self-contained.
static const PROPERTYKEY kPkeyDeviceFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const PROPERTYKEY kPkeyDeviceDesc         = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 2 };
static const PROPERTYKEY kPkeyInterfaceName      = { { 0xb3f8fa53, 0x0004, 0x438e, { 0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc } }, 6 };
static const PROPERTYKEY kPkeyDeviceFormat       = { { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };
static const PROPERTYKEY kPkeyOemFormat          = { { 0xe4870e26, 0x3cc5, 0x4cd2, { 0xba, 0x46, 0xca, 0x0a, 0x9a, 0x70, 0xed, 0x04 } }, 3 };

namespace {

bool MakeEnumerator(ComPtr<IMMDeviceEnumerator>& out) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&out));
    return SUCCEEDED(hr) && out;
}

bool MakePolicyConfig(ComPtr<IPolicyConfig>& out) {
    HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&out));
    return SUCCEEDED(hr) && out;
}

std::string Hresult(const char* what, HRESULT hr) {
    char buf[128];
    sprintf_s(buf, "%s failed (0x%08lX)", what, (unsigned long)hr);
    return buf;
}

// Reads the endpoint's stored default format (the blob the control panel's
// "Default Format" dropdown edits) into a byte buffer.
bool ReadDeviceFormat(const std::wstring& deviceId, std::vector<BYTE>& out) {
    ComPtr<IMMDeviceEnumerator> enumr;
    if (!MakeEnumerator(enumr)) return false;

    ComPtr<IMMDevice> device;
    if (FAILED(enumr->GetDevice(deviceId.c_str(), &device)) || !device) return false;

    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) return false;

    PROPVARIANT pv;
    PropVariantInit(&pv);
    bool ok = false;
    if (SUCCEEDED(store->GetValue(kPkeyDeviceFormat, &pv)) && pv.vt == VT_BLOB &&
        pv.blob.pBlobData && pv.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        out.assign(pv.blob.pBlobData, pv.blob.pBlobData + pv.blob.cbSize);
        ok = true;
    }
    PropVariantClear(&pv);
    return ok;
}

// Fallback path for SetSampleRate: write the format blob straight into the
// endpoint property store. Less immediate than IPolicyConfig (the audio engine
// picks it up when the endpoint is next reloaded) but it survives if the
// undocumented interface ever disappears.
bool WriteDeviceFormat(const std::wstring& deviceId, const std::vector<BYTE>& fmt, std::string* outError) {
    ComPtr<IMMDeviceEnumerator> enumr;
    if (!MakeEnumerator(enumr)) {
        if (outError) *outError = "no device enumerator";
        return false;
    }

    ComPtr<IMMDevice> device;
    HRESULT hr = enumr->GetDevice(deviceId.c_str(), &device);
    if (FAILED(hr) || !device) {
        if (outError) *outError = Hresult("GetDevice", hr);
        return false;
    }

    ComPtr<IPropertyStore> store;
    hr = device->OpenPropertyStore(STGM_READWRITE, &store);
    if (FAILED(hr) || !store) {
        if (outError) *outError = Hresult("OpenPropertyStore(write)", hr);
        return false;
    }

    // The PROPVARIANT borrows our buffer, so it must not be PropVariantClear'd
    // (that would CoTaskMemFree memory we own).
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = (ULONG)fmt.size();
    pv.blob.pBlobData = const_cast<BYTE*>(fmt.data());

    hr = store->SetValue(kPkeyDeviceFormat, pv);
    if (SUCCEEDED(hr)) {
        store->SetValue(kPkeyOemFormat, pv); // best effort: keeps both in sync
        hr = store->Commit();
    }

    pv.blob.cbSize = 0;
    pv.blob.pBlobData = nullptr;
    pv.vt = VT_EMPTY;

    if (FAILED(hr)) {
        if (outError) *outError = Hresult("SetValue(DeviceFormat)", hr);
        return false;
    }
    return true;
}

// PKEY_Device_FriendlyName ("Speakers (FiiO K7)") is synthesised by MMDevice
// rather than stored, so if it ever comes back empty fall back to the two parts
// it is built from — the user's match string is compared against this name.
std::string ReadDeviceName(IPropertyStore* store) {
    const PROPERTYKEY keys[] = { kPkeyDeviceFriendlyName, kPkeyInterfaceName, kPkeyDeviceDesc };
    for (const auto& key : keys) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::string name;
        if (SUCCEEDED(store->GetValue(key, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
            name = ToUtf8(pv.pwszVal);
        }
        PropVariantClear(&pv);
        if (!name.empty()) return name;
    }
    return {};
}

int RateFromBlob(const std::vector<BYTE>& fmt) {
    if (fmt.size() < sizeof(WAVEFORMATEX)) return 0;
    return (int)reinterpret_cast<const WAVEFORMATEX*>(fmt.data())->nSamplesPerSec;
}

} // namespace

namespace AudioDevices {

std::vector<RenderDeviceInfo> ListRenderDevices() {
    std::vector<RenderDeviceInfo> devices;

    ComPtr<IMMDeviceEnumerator> enumr;
    if (!MakeEnumerator(enumr)) return devices;

    std::wstring defaultId;
    {
        ComPtr<IMMDevice> defDevice;
        if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(eRender, eMultimedia, &defDevice)) && defDevice) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(defDevice->GetId(&id)) && id) {
                defaultId = id;
                CoTaskMemFree(id);
            }
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) || !collection) {
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device) continue;

        RenderDeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            info.id = id;
            CoTaskMemFree(id);
        }
        if (info.id.empty()) continue;
        info.isDefault = (info.id == defaultId);

        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store) {
            info.name = ReadDeviceName(store.Get());

            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(store->GetValue(kPkeyDeviceFormat, &pv)) && pv.vt == VT_BLOB &&
                pv.blob.pBlobData && pv.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                const WAVEFORMATEX* wf = reinterpret_cast<const WAVEFORMATEX*>(pv.blob.pBlobData);
                info.sampleRate = (int)wf->nSamplesPerSec;
                info.bitDepth = (int)wf->wBitsPerSample;
                info.channels = (int)wf->nChannels;
            }
            PropVariantClear(&pv);
        }

        if (info.name.empty()) info.name = "(unnamed device)";
        devices.push_back(info);
    }

    return devices;
}

bool ResolveDevice(const std::string& nameMatch, RenderDeviceInfo& out) {
    std::vector<RenderDeviceInfo> devices = ListRenderDevices();
    if (devices.empty()) return false;

    if (nameMatch.empty()) {
        for (const auto& d : devices) {
            if (d.isDefault) { out = d; return true; }
        }
        return false;
    }

    for (const auto& d : devices) {
        if (ContainsNoCase(d.name, nameMatch)) { out = d; return true; }
    }
    return false;
}

bool GetSampleRate(const std::wstring& deviceId, int& outHz) {
    std::vector<BYTE> fmt;
    if (!ReadDeviceFormat(deviceId, fmt)) return false;
    int hz = RateFromBlob(fmt);
    if (hz <= 0) return false;
    outHz = hz;
    return true;
}

bool SetSampleRate(const std::wstring& deviceId, int hz, std::string* outError, bool* outAppliedLive) {
    if (outAppliedLive) *outAppliedLive = false;
    if (hz <= 0) {
        if (outError) *outError = "invalid sample rate";
        return false;
    }

    // Start from the device's own format so only the rate changes — guessing a
    // full format risks handing the DAC something it does not accept.
    std::vector<BYTE> fmt;
    if (!ReadDeviceFormat(deviceId, fmt)) {
        if (outError) *outError = "cannot read the device's current format";
        return false;
    }

    WAVEFORMATEX* wf = reinterpret_cast<WAVEFORMATEX*>(fmt.data());
    if (wf->nChannels == 0 || wf->wBitsPerSample == 0) {
        if (outError) *outError = "device reported an unusable format";
        return false;
    }
    if ((int)wf->nSamplesPerSec == hz) {
        if (outAppliedLive) *outAppliedLive = true;
        return true; // already there
    }

    wf->nSamplesPerSec = (DWORD)hz;
    wf->nBlockAlign = (WORD)(wf->nChannels * (wf->wBitsPerSample / 8));
    wf->nAvgBytesPerSec = wf->nSamplesPerSec * wf->nBlockAlign;

    // Preferred path: the exact call the Sound control panel makes, so the
    // audio engine applies the new rate to the live endpoint immediately.
    ComPtr<IPolicyConfig> policy;
    std::string policyError;
    if (MakePolicyConfig(policy)) {
        HRESULT hr = policy->SetDeviceFormat(deviceId.c_str(), wf, wf);
        if (SUCCEEDED(hr)) {
            if (outAppliedLive) *outAppliedLive = true;
            if (outError) outError->clear();
            return true;
        }
        policyError = Hresult("SetDeviceFormat", hr);
    } else {
        policyError = "IPolicyConfig unavailable";
    }

    // Fallback: writes the stored default, but on this Windows build has not
    // been observed to reach the already-running audio engine — surface the
    // real reason the live path was skipped instead of hiding it on success.
    std::string fallbackError;
    if (WriteDeviceFormat(deviceId, fmt, &fallbackError)) {
        if (outAppliedLive) *outAppliedLive = false;
        if (outError) *outError = "live apply unavailable (" + policyError + "); wrote stored default only";
        return true;
    }
    if (outError) *outError = policyError + "; " + fallbackError;
    return false;
}

bool GetDefaultRenderDeviceId(std::wstring& outId) {
    ComPtr<IMMDeviceEnumerator> enumr;
    if (!MakeEnumerator(enumr)) return false;

    ComPtr<IMMDevice> device;
    if (FAILED(enumr->GetDefaultAudioEndpoint(eRender, eMultimedia, &device)) || !device) return false;

    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return false;
    outId = id;
    CoTaskMemFree(id);
    return !outId.empty();
}

bool SetDefaultRenderDevice(const std::wstring& deviceId, std::string* outError) {
    if (deviceId.empty()) {
        if (outError) *outError = "empty device id";
        return false;
    }

    ComPtr<IPolicyConfig> policy;
    if (!MakePolicyConfig(policy)) {
        if (outError) *outError = "IPolicyConfig unavailable";
        return false;
    }

    // All three roles, same as clicking "Set as Default Device" in the Sound
    // control panel (which also covers the Communications role).
    const ERole roles[] = { eConsole, eMultimedia, eCommunications };
    HRESULT last = S_OK;
    bool any = false;
    for (ERole role : roles) {
        HRESULT hr = policy->SetDefaultEndpoint(deviceId.c_str(), role);
        if (SUCCEEDED(hr)) any = true;
        else last = hr;
    }

    if (!any) {
        if (outError) *outError = Hresult("SetDefaultEndpoint", last);
        return false;
    }

    // Volume control caches the WASAPI chain for the *default* endpoint; the
    // device it points at has just changed underneath it.
    AudioManager::InvalidateDeviceCache();
    return true;
}

} // namespace AudioDevices
