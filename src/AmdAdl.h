#pragma once
#include <windows.h>

// Minimal subset of AMD's public ADL (AMD Display Library) SDK, reproduced
// from github.com/GPUOpen-LibrariesAndSDKs/display-library (MIT license) —
// only the handful of struct fields and constants this app actually reads.
// AMD's real headers total tens of thousands of lines covering overclocking,
// Eyefinity, color management, etc.; none of that is needed here, so this
// hand-picked subset stands in for them the same way PolicyConfig.h stands in
// for the undocumented COM interface rather than pulling in a full SDK.
//
// ADL itself is never linked at build time: AmdDdcSideChannel.cpp loads
// atiadlxx.dll via LoadLibrary at runtime, so a machine with no AMD driver
// simply fails that LoadLibrary call and this feature no-ops.

typedef void* (__stdcall* ADL_MAIN_MALLOC_CALLBACK)(int);

#define ADL_OK 0
#define ADL_MAX_PATH 256

#define ADL_DISPLAY_DISPLAYINFO_DISPLAYCONNECTED 0x00000001
#define ADL_DISPLAY_DISPLAYINFO_DISPLAYMAPPED    0x00000002

#pragma pack(push, 8)

typedef struct AdapterInfo {
    int iSize;
    int iAdapterIndex;
    char strUDID[ADL_MAX_PATH];
    int iBusNumber;
    int iDeviceNumber;
    int iFunctionNumber;
    int iVendorID;
    char strAdapterName[ADL_MAX_PATH];
    char strDisplayName[ADL_MAX_PATH];
    int iPresent;
    int iExist;
    char strDriverPath[ADL_MAX_PATH];
    char strDriverPathExt[ADL_MAX_PATH];
    char strPNPString[ADL_MAX_PATH];
    int iOSDisplayIndex;
} AdapterInfo, *LPAdapterInfo;

typedef struct ADLDisplayID {
    int iDisplayLogicalIndex;
    int iDisplayPhysicalIndex;
    int iDisplayLogicalAdapterIndex;
    int iDisplayPhysicalAdapterIndex;
} ADLDisplayID, *LPADLDisplayID;

typedef struct ADLDisplayInfo {
    ADLDisplayID displayID;
    int iDisplayControllerIndex;
    char strDisplayName[ADL_MAX_PATH];
    char strDisplayManufacturerName[ADL_MAX_PATH];
    int iDisplayType;
    int iDisplayOutputType;
    int iDisplayConnector;
    int iDisplayInfoMask;
    int iDisplayInfoValue;
} ADLDisplayInfo, *LPADLDisplayInfo;

#pragma pack(pop)

typedef int (*ADL_MAIN_CONTROL_CREATE)(ADL_MAIN_MALLOC_CALLBACK, int);
typedef int (*ADL_MAIN_CONTROL_DESTROY)();
typedef int (*ADL_ADAPTER_NUMBEROFADAPTERS_GET)(int*);
typedef int (*ADL_ADAPTER_ADAPTERINFO_GET)(LPAdapterInfo, int);
typedef int (*ADL_DISPLAY_DISPLAYINFO_GET)(int, int*, ADLDisplayInfo**, int);
// DDC "Block Access": sends a raw byte buffer over the DDC/CI channel and
// (optionally) reads a reply. See ADL (AMD Display Library) Public APIs online
// docs for the full signature; this app only ever writes (recv buffer null).
typedef int (*ADL_DISPLAY_DDCBLOCKACCESSGET)(int iAdapterIndex, int iDisplayIndex, int iOption,
                                              int iCommandIndex, int iSendMsgLen, char* lpucSendMsgBuf,
                                              int* lpulRecvMsgLen, char* lpucRecvMsgBuf);
