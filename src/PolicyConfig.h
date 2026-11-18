#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

// IPolicyConfig is the undocumented interface the Windows Sound control panel
// itself uses for two things this app needs:
//   * SetDeviceFormat   -> the "Default Format" dropdown on the Advanced tab
//                          (i.e. the endpoint sample rate / bit depth)
//   * SetDefaultEndpoint-> the "Set as Default Device" button
// No SDK header declares it, so it is reproduced here. It is a pure vtable
// contract: the method ORDER below is what AudioSes.dll ships and must not be
// reordered or have entries removed, otherwise calls land on the wrong slot.
// Signatures that this app never calls keep their arguments as void* on
// purpose — their real parameter types are undocumented structs.

struct DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR deviceId, WAVEFORMATEX** ppFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR deviceId, INT bDefault, WAVEFORMATEX** ppFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR deviceId) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR deviceId, WAVEFORMATEX* pEndpointFormat, WAVEFORMATEX* pMixFormat) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR deviceId, INT bDefault, PINT64 pmftDefaultPeriod, PINT64 pmftMinimumPeriod) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR deviceId, PINT64 pmftPeriod) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR deviceId, void* pMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR deviceId, void* pMode) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR deviceId, const PROPERTYKEY& key, PROPVARIANT* pv) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR deviceId, const PROPERTYKEY& key, PROPVARIANT* pv) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR deviceId, INT bVisible) = 0;
};

// CLSID_CPolicyConfigClient — only declared so __uuidof() resolves; there is no
// definition to link against.
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;
