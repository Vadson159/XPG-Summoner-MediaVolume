#include "AmdDdcSideChannel.h"
#include "AmdAdl.h"
#include "StrUtil.h"

#include <setupapi.h>
#include <devguid.h>
#include <algorithm>
#include <vector>

#pragma comment(lib, "setupapi.lib")

namespace {

// Directories of the driver packages the present display adapters are
// actually bound to, taken from each adapter's driver key (UserModeDriverName
// lists full DriverStore paths of the UMD DLLs, which ship next to ADL).
std::vector<std::wstring> ActiveDisplayDriverDirs() {
    std::vector<std::wstring> dirs;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return dirs;

    SP_DEVINFO_DATA dev{ sizeof(dev) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        HKEY key = SetupDiOpenDevRegKey(set, &dev, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;

        wchar_t buf[2048] = {};
        DWORD size = sizeof(buf) - 2 * sizeof(wchar_t); // keep a double terminator
        DWORD type = 0;
        LSTATUS rc = RegQueryValueExW(key, L"UserModeDriverName", nullptr, &type, (BYTE*)buf, &size);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS || type != REG_MULTI_SZ) continue;

        std::wstring first = buf;
        size_t slash = first.find_last_of(L'\\');
        if (slash == std::wstring::npos) continue;
        std::wstring dir = first.substr(0, slash);
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    }
    SetupDiDestroyDeviceInfoList(set);
    return dirs;
}

// An ADL build only sees the displays of the GPUs running *its own* driver
// package: every adapter is still listed, but those bound to another package
// report zero displays, which surfaces as "no ADL display matching". Two ways
// this was hit on the same machine:
//  - System32\atiadlxx.dll came from a newer, staged package the GPUs were not
//    bound to, so it saw no display at all;
//  - after a driver update the iGPU and the dGPU ended up on different
//    packages, so each package's ADL saw only its own GPU's monitor.
// So try the copy from every bound package's DriverStore folder (deduped), then
// the System32 one, and let the caller use whichever finds the monitor.
std::vector<std::wstring> AdlCandidates() {
    std::vector<std::wstring> paths;
    for (const std::wstring& dir : ActiveDisplayDriverDirs()) {
        std::wstring path = dir + L"\\atiadlxx.dll";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) paths.push_back(path);
    }
    paths.push_back(L"atiadlxx.dll");
    paths.push_back(L"atiadlxy.dll"); // 32-bit process on 64-bit OS
    return paths;
}

HMODULE LoadAdlLibrary(const std::wstring& path) {
    // A bare name searches as usual; a full path also resolves the DLL's own
    // dependencies from its DriverStore folder.
    return path.find(L'\\') == std::wstring::npos
        ? LoadLibraryW(path.c_str())
        : LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

// Loaded fresh on every call rather than kept as persistent state, matching
// AudioDevices.cpp's "local per call" convention — this runs at most a couple
// of times per console/PC swap, so the LoadLibrary + enumerate cost (a few ms)
// is not worth adding lifecycle/cleanup complexity for.
struct AdlSession {
    HMODULE dll = nullptr;
    ADL_MAIN_CONTROL_CREATE create = nullptr;
    ADL_MAIN_CONTROL_DESTROY destroy = nullptr;
    ADL_ADAPTER_NUMBEROFADAPTERS_GET numAdapters = nullptr;
    ADL_ADAPTER_ADAPTERINFO_GET adapterInfo = nullptr;
    ADL_DISPLAY_DISPLAYINFO_GET displayInfo = nullptr;
    ADL_DISPLAY_DDCBLOCKACCESSGET ddcBlockAccess = nullptr;
    bool created = false;

    ~AdlSession() {
        if (created && destroy) destroy();
        if (dll) FreeLibrary(dll);
    }
};

void* __stdcall AdlAlloc(int size) { return malloc(size); }

bool LoadAdl(AdlSession& s, const std::wstring& path, std::string* outError) {
    s.dll = LoadAdlLibrary(path);
    if (!s.dll) {
        if (outError) *outError = "AMD ADL unavailable (atiadlxx.dll not found - no AMD GPU driver?)";
        return false;
    }

    s.create = (ADL_MAIN_CONTROL_CREATE)GetProcAddress(s.dll, "ADL_Main_Control_Create");
    s.destroy = (ADL_MAIN_CONTROL_DESTROY)GetProcAddress(s.dll, "ADL_Main_Control_Destroy");
    s.numAdapters = (ADL_ADAPTER_NUMBEROFADAPTERS_GET)GetProcAddress(s.dll, "ADL_Adapter_NumberOfAdapters_Get");
    s.adapterInfo = (ADL_ADAPTER_ADAPTERINFO_GET)GetProcAddress(s.dll, "ADL_Adapter_AdapterInfo_Get");
    s.displayInfo = (ADL_DISPLAY_DISPLAYINFO_GET)GetProcAddress(s.dll, "ADL_Display_DisplayInfo_Get");
    s.ddcBlockAccess = (ADL_DISPLAY_DDCBLOCKACCESSGET)GetProcAddress(s.dll, "ADL_Display_DDCBlockAccess_Get");

    if (!s.create || !s.destroy || !s.numAdapters || !s.adapterInfo || !s.displayInfo || !s.ddcBlockAccess) {
        if (outError) *outError = "AMD ADL loaded but is missing an expected entry point";
        return false;
    }

    if (s.create(AdlAlloc, 1) != ADL_OK) {
        if (outError) *outError = "ADL_Main_Control_Create failed";
        return false;
    }
    s.created = true;
    return true;
}

struct AdlDisplay {
    int adapterIndex;
    int displayIndex;
    std::string name;
};

// seen accumulates the connected display names across calls, for the error
// message when no candidate ADL finds the monitor.
bool FindDisplay(AdlSession& s, const std::string& nameMatch, AdlDisplay& out, std::string& seen) {
    int numAdapters = 0;
    s.numAdapters(&numAdapters);
    if (numAdapters <= 0) return false;

    std::vector<AdapterInfo> adapters((size_t)numAdapters);
    memset(adapters.data(), 0, sizeof(AdapterInfo) * numAdapters);
    s.adapterInfo(adapters.data(), (int)(sizeof(AdapterInfo) * numAdapters));

    for (int i = 0; i < numAdapters; ++i) {
        int adapterIndex = adapters[(size_t)i].iAdapterIndex;
        ADLDisplayInfo* displays = nullptr;
        int numDisplays = 0;
        if (s.displayInfo(adapterIndex, &numDisplays, &displays, 0) != ADL_OK || !displays) continue;

        for (int j = 0; j < numDisplays; ++j) {
            const int mask = ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED | ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED;
            if ((displays[j].iDisplayInfoValue & mask) != mask) continue;
            if (displays[j].displayID.iDisplayLogicalAdapterIndex != adapterIndex) continue;

            if (!ContainsNoCase(seen, displays[j].strDisplayName))
                seen += std::string(seen.empty() ? "" : ", ") + "\"" + displays[j].strDisplayName + "\"";
            if (ContainsNoCase(displays[j].strDisplayName, nameMatch)) {
                out.adapterIndex = adapterIndex;
                out.displayIndex = displays[j].displayID.iDisplayLogicalIndex;
                out.name = displays[j].strDisplayName;
                free(displays);
                return true;
            }
        }
        free(displays);
    }
    return false;
}

// "Set VCP Feature" packet, byte-identical to the standard MCCS command
// except for the source-address byte (regAddr) and feature code (vcpCode),
// which is the entire trick this side channel relies on. Checksum matches
// both AMD's and NVIDIA's own official sample code: XOR of every preceding
// byte including the leading I2C address byte.
bool SendSetVcp(AdlSession& s, int adapterIndex, int displayIndex, BYTE regAddr, BYTE vcpCode, int value) {
    unsigned char pkt[8] = {
        0x6E, regAddr, 0x84, 0x03, vcpCode,
        (unsigned char)((value >> 8) & 0xFF), (unsigned char)(value & 0xFF), 0x00
    };
    unsigned char checksum = 0;
    for (int i = 0; i < 7; ++i) checksum ^= pkt[i];
    pkt[7] = checksum;

    int recvLen = 0;
    return s.ddcBlockAccess(adapterIndex, displayIndex, 0, 0, sizeof(pkt), (char*)pkt, &recvLen, nullptr) == ADL_OK;
}

} // namespace

namespace AmdDdcSideChannel {

bool IsAvailable() {
    for (const std::wstring& path : AdlCandidates()) {
        if (HMODULE dll = LoadAdlLibrary(path)) {
            FreeLibrary(dll);
            return true;
        }
    }
    return false;
}

bool SetInput(const std::string& nameMatch, int value, std::string* outError) {
    if (outError) outError->clear();
    if (nameMatch.empty()) {
        if (outError) *outError = "no monitor selected";
        return false;
    }

    std::string seen, loadError;
    bool anyLoaded = false;
    for (const std::wstring& path : AdlCandidates()) {
        AdlSession session;
        if (!LoadAdl(session, path, &loadError)) continue;
        anyLoaded = true;

        AdlDisplay display;
        if (!FindDisplay(session, nameMatch, display, seen)) continue;

        // 0x50 is the LG service side-channel's source-address byte (0x51 is the
        // normal DDC/CI channel); 0xF4 is its input-select feature code (0x60 on
        // the normal channel).
        if (!SendSetVcp(session, display.adapterIndex, display.displayIndex, 0x50, 0xF4, value)) {
            if (outError) *outError = "ADL_Display_DDCBlockAccess_Get failed for \"" + display.name + "\"";
            return false;
        }
        return true;
    }

    if (outError) {
        if (!anyLoaded)
            *outError = loadError;
        else if (seen.empty())
            *outError = "ADL reports no connected displays - no installed atiadlxx.dll matches the running AMD driver";
        else
            *outError = "no ADL display matching \"" + nameMatch + "\" (ADL sees " + seen + ")";
    }
    return false;
}

} // namespace AmdDdcSideChannel
