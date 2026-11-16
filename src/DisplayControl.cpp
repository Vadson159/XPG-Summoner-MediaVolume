#include "DisplayControl.h"
#include "StrUtil.h"

#include <physicalmonitorenumerationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <dwmapi.h>

#pragma comment(lib, "dxva2.lib")
#pragma comment(lib, "user32.lib")

// MCCS "Input Source" feature. Common values: 0x01 VGA-1, 0x03 DVI-1,
// 0x04 DVI-2, 0x0F DisplayPort-1, 0x10 DisplayPort-2, 0x11 HDMI-1, 0x12 HDMI-2.
// Monitors are free to deviate, which is why the settings menu shows the value
// the monitor currently reports instead of hardcoding a table.
static const BYTE kVcpInputSource = 0x60;

namespace {

// Wire layout from wingdi.h's DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 and
// DISPLAYCONFIG_SET_HDR_STATE. Local packets also build with older Windows
// SDKs; older OS versions reject type 15 and use the legacy HDR API below.
// The newer API distinguishes HDR from SDR automatic color management (WCG).
struct HdrInfo2Packet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 flags;
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;
};
struct HdrStatePacket {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 enableHdr;
};
static_assert(sizeof(HdrInfo2Packet) == 36 && sizeof(HdrStatePacket) == 24);

struct HdrDisplay {
    LUID adapter;
    UINT32 id;
    bool modern = false;
    bool supported = false;
    bool enabled = false;
    bool limitedByPolicy = false;
    std::string name;
};

LONG ReadHdrState(HdrDisplay& display) {
    HdrInfo2Packet info = {};
    info.header = {static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15), sizeof(info), display.adapter, display.id};
    LONG status = DisplayConfigGetDeviceInfo(&info.header);
    if (status == ERROR_SUCCESS) {
        display.modern = true;
        display.supported = (info.flags & (1u << 4)) != 0; // highDynamicRangeSupported
        display.enabled = (info.flags & (1u << 5)) != 0;   // highDynamicRangeUserEnabled
        display.limitedByPolicy = (info.flags & (1u << 3)) != 0;
        return status;
    }
    if (status != ERROR_INVALID_PARAMETER && status != ERROR_NOT_SUPPORTED) return status;

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacy = {};
    legacy.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(legacy), display.adapter, display.id};
    status = DisplayConfigGetDeviceInfo(&legacy.header);
    if (status == ERROR_SUCCESS) {
        display.modern = false;
        display.supported = legacy.advancedColorSupported && !legacy.wideColorEnforced;
        display.enabled = legacy.advancedColorEnabled && !legacy.wideColorEnforced;
        display.limitedByPolicy = legacy.advancedColorForceDisabled != 0;
    }
    return status;
}

LONG WriteHdrState(const HdrDisplay& display, bool enable) {
    if (display.modern) {
        HdrStatePacket state = {};
        state.header = {static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(16), sizeof(state), display.adapter, display.id};
        state.enableHdr = enable ? 1u : 0u;
        return DisplayConfigSetDeviceInfo(&state.header);
    }
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state = {};
    state.header = {DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE, sizeof(state), display.adapter, display.id};
    state.enableAdvancedColor = enable ? 1u : 0u;
    return DisplayConfigSetDeviceInfo(&state.header);
}

LONG QueryActivePaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths) {
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        UINT32 pathCount = 0, modeCount = 0;
        status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (status != ERROR_SUCCESS) break;
        paths.resize(pathCount);
        modes.resize(modeCount);
        status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                    &modeCount, modes.data(), nullptr);
        if (status == ERROR_SUCCESS) paths.resize(pathCount);
    }
    return status;
}

struct EnumContext {
    std::vector<MonitorInfo>* out;
    bool queryInputs;
};

void FillGdiInfo(MonitorInfo& info, const wchar_t* gdiName) {
    DISPLAY_DEVICEW monitorDev = {};
    monitorDev.cb = sizeof(monitorDev);
    if (EnumDisplayDevicesW(gdiName, 0, &monitorDev, 0)) {
        info.deviceId = ToUtf8(monitorDev.DeviceID);
        info.description = ToUtf8(monitorDev.DeviceString);
    }

    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsExW(gdiName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        info.width = (int)dm.dmPelsWidth;
        info.height = (int)dm.dmPelsHeight;
        info.refreshHz = (int)dm.dmDisplayFrequency;
    }
}

void FillDdcInfo(MonitorInfo& info, HMONITOR hMonitor) {
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count) || count == 0) return;

    std::vector<PHYSICAL_MONITOR> monitors(count);
    if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, count, monitors.data())) return;

    // One HMONITOR maps to one panel in every setup this app cares about, so
    // only the first physical monitor is inspected.
    MC_VCP_CODE_TYPE type = MC_MOMENTARY;
    DWORD current = 0, maximum = 0;
    if (GetVCPFeatureAndVCPFeatureReply(monitors[0].hPhysicalMonitor, kVcpInputSource, &type, &current, &maximum)) {
        info.ddcOk = true;
        info.currentInput = (int)current;
        info.maxInput = (int)maximum;
    }

    std::string ddcName = ToUtf8(monitors[0].szPhysicalMonitorDescription);
    if (!ddcName.empty() && info.description.empty()) info.description = ddcName;

    DestroyPhysicalMonitors(count, monitors.data());
}

BOOL CALLBACK EnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) {
    EnumContext* ctx = reinterpret_cast<EnumContext*>(lParam);

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) return TRUE;

    MonitorInfo info;
    info.deviceName = ToUtf8(mi.szDevice);
    info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    FillGdiInfo(info, mi.szDevice);
    if (ctx->queryInputs) FillDdcInfo(info, hMonitor);

    ctx->out->push_back(info);
    return TRUE;
}

// Runs body() against the first matching physical monitor.
template <typename Body>
bool WithMatchingPhysicalMonitor(const std::string& match, std::string* outError,
                                 Body body) {
    std::vector<MonitorInfo> monitors = DisplayControl::ListMonitors(false);

    for (const auto& monitor : monitors) {
        if (!DisplayControl::Matches(monitor, match)) continue;

        // Re-resolve the HMONITOR from the matched GDI name: EnumDisplayMonitors
        // does not hand out a handle we may keep.
        POINT probe = { 0, 0 };
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        std::wstring nameW = ToWide(monitor.deviceName);
        if (!EnumDisplaySettingsExW(nameW.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) continue;
        probe.x = dm.dmPosition.x + 1;
        probe.y = dm.dmPosition.y + 1;

        HMONITOR hMonitor = MonitorFromPoint(probe, MONITOR_DEFAULTTONULL);
        if (!hMonitor) continue;

        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, &count) || count == 0) {
            if (outError) *outError = "monitor does not expose DDC/CI";
            continue;
        }

        std::vector<PHYSICAL_MONITOR> physical(count);
        if (!GetPhysicalMonitorsFromHMONITOR(hMonitor, count, physical.data())) {
            if (outError) *outError = "GetPhysicalMonitorsFromHMONITOR failed";
            continue;
        }

        bool ok = body(physical[0].hPhysicalMonitor);
        DestroyPhysicalMonitors(count, physical.data());
        return ok;
    }

    if (outError && outError->empty()) *outError = "no monitor matched \"" + match + "\"";
    return false;
}

// Windows that belong to the shell or to this app itself must stay put.
bool IsMovableWindow(HWND hwnd) {
    if (!IsWindowVisible(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false; // dialogs follow their owner
    if (GetWindowTextLengthW(hwnd) == 0) return false;      // untitled -> not a user window

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;

    // UWP keeps invisible "cloaked" host windows around; moving them is pointless.
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) {
        return false;
    }

    wchar_t cls[128] = {};
    GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0])));
    const wchar_t* skip[] = { L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Progman", L"WorkerW",
                              L"Windows.UI.Core.CoreWindow", L"SmartMixerClass" };
    for (const wchar_t* s : skip) {
        if (_wcsicmp(cls, s) == 0) return false;
    }
    return true;
}

BOOL CALLBACK SnapshotProc(HWND hwnd, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<DisplayControl::MovedWindowState>*>(lParam);
    if (!IsMovableWindow(hwnd)) return TRUE;
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return TRUE;
    out->push_back({ hwnd, wp });
    return TRUE;
}

// Moves one window onto dst (a monitor's work area), scaled proportionally
// from its position within src (its current monitor's work area). Returns
// false without touching the window if the geometry is degenerate.
bool ScaleWindowInto(HWND hwnd, const WINDOWPLACEMENT& current, const RECT& src, const RECT& dst) {
    const LONG srcW = src.right - src.left, srcH = src.bottom - src.top;
    const LONG dstW = dst.right - dst.left, dstH = dst.bottom - dst.top;
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return false;

    const double fx = (double)dstW / (double)srcW;
    const double fy = (double)dstH / (double)srcH;
    const RECT r = current.rcNormalPosition;
    LONG w = (LONG)((r.right - r.left) * fx + 0.5);
    LONG h = (LONG)((r.bottom - r.top) * fy + 0.5);
    if (w > dstW) w = dstW;
    if (h > dstH) h = dstH;
    LONG x = dst.left + (LONG)((r.left - src.left) * fx + 0.5);
    LONG y = dst.top + (LONG)((r.top - src.top) * fy + 0.5);
    if (x + w > dst.right) x = dst.right - w;
    if (y + h > dst.bottom) y = dst.bottom - h;
    if (x < dst.left) x = dst.left;
    if (y < dst.top) y = dst.top;

    WINDOWPLACEMENT wp = current;
    wp.rcNormalPosition.left = x;
    wp.rcNormalPosition.top = y;
    wp.rcNormalPosition.right = x + w;
    wp.rcNormalPosition.bottom = y + h;

    // A maximized window will NOT follow a changed rcNormalPosition - Windows
    // keeps it maximized on the monitor it is already on and only remembers the
    // new restore rect (measured: maximized windows stayed put while normal ones
    // moved). It has to be un-maximized onto the target first, then re-maximized
    // there.
    const bool wasMaximized = (wp.showCmd == SW_SHOWMAXIMIZED);
    if (wasMaximized) wp.showCmd = SW_SHOWNORMAL;

    if (!SetWindowPlacement(hwnd, &wp)) return false;
    if (wasMaximized) ShowWindow(hwnd, SW_SHOWMAXIMIZED);
    return true;
}

bool DoSetInput(HANDLE hPhysical, int value, std::string* outError) {
    if (!SetVCPFeature(hPhysical, kVcpInputSource, (DWORD)value)) {
        if (outError) {
            char buf[128];
            sprintf_s(buf, "SetVCPFeature(0x60, %d) rejected - is DDC/CI enabled in the monitor OSD?", value);
            *outError = buf;
        }
        return false;
    }

    // SetVCPFeature returning success only means the DDC/CI transaction
    // completed - some monitors ack the write and then never actually switch
    // (commonly because "DDC/CI" is a separate on/off toggle in the monitor's
    // own menu that only gates writes, or the firmware just doesn't support
    // remote input switching despite advertising 0x60 as readable). Reading
    // the value back is the only way to catch that silent-failure case instead
    // of reporting false success.
    Sleep(400);
    MC_VCP_CODE_TYPE type = MC_MOMENTARY;
    DWORD current = 0, maximum = 0;
    if (GetVCPFeatureAndVCPFeatureReply(hPhysical, kVcpInputSource, &type, &current, &maximum) &&
        (int)current != value) {
        if (outError) {
            char buf[160];
            sprintf_s(buf, "monitor acknowledged the command but stayed on 0x%02X - check that DDC/CI input control is enabled in its own menu", (int)current);
            *outError = buf;
        }
        return false;
    }
    // If the read-back itself fails, don't fault the write for it - some
    // monitors briefly stop answering DDC/CI mid-switch.
    return true;
}

} // namespace

namespace DisplayControl {

std::vector<MonitorInfo> ListMonitors(bool queryInputs) {
    std::vector<MonitorInfo> monitors;
    EnumContext ctx{ &monitors, queryInputs };
    EnumDisplayMonitors(nullptr, nullptr, EnumProc, reinterpret_cast<LPARAM>(&ctx));
    return monitors;
}

bool Matches(const MonitorInfo& monitor, const std::string& match) {
    if (match.empty()) return false;
    return ContainsNoCase(monitor.deviceId, match) ||
           ContainsNoCase(monitor.description, match) ||
           ContainsNoCase(monitor.deviceName, match);
}

bool SetMonitorInput(const std::string& match, int vcpValue, std::string* outError) {
    if (outError) outError->clear();
    if (match.empty()) {
        if (outError) *outError = "no monitor selected";
        return false;
    }
    return WithMatchingPhysicalMonitor(match, outError, [&](HANDLE monitor) {
        return DoSetInput(monitor, vcpValue, outError);
    });
}

bool GetMonitorPictureMode(const std::string& match, int& outMode, std::string* outError) {
    if (outError) outError->clear();
    return WithMatchingPhysicalMonitor(match, outError, [&](HANDLE monitor) {
        MC_VCP_CODE_TYPE type;
        DWORD current = 0, maximum = 0;
        if (!GetVCPFeatureAndVCPFeatureReply(monitor, 0x15, &type, &current, &maximum)) {
            if (outError) *outError = "cannot read the monitor picture mode";
            return false;
        }
        outMode = static_cast<int>(current);
        return true;
    });
}

bool SetMonitorPictureMode(const std::string& match, int mode, std::string* outError) {
    if (outError) outError->clear();
    if (mode < 0 || mode > 255) {
        if (outError) *outError = "invalid picture mode";
        return false;
    }
    return WithMatchingPhysicalMonitor(match, outError, [&](HANDLE monitor) {
        if (!SetVCPFeature(monitor, 0x15, static_cast<DWORD>(mode))) {
            if (outError) *outError = "monitor rejected the picture mode";
            return false;
        }
        for (int attempt = 0; attempt < 5; ++attempt) {
            Sleep(150); // the monitor needs time to apply its new profile
            MC_VCP_CODE_TYPE type;
            DWORD current = 0, maximum = 0;
            if (GetVCPFeatureAndVCPFeatureReply(monitor, 0x15, &type, &current, &maximum) &&
                current == static_cast<DWORD>(mode)) return true;
        }
        if (outError) *outError = "monitor did not confirm the picture mode change";
        return false;
    });
}

bool GetMonitorHdrEnabled(const std::string& gdiName, bool& enabled, std::string* outError) {
    if (outError) outError->clear();
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    const LONG status = QueryActivePaths(paths);
    if (status == ERROR_SUCCESS) {
        const auto nameW = ToWide(gdiName);
        for (const auto& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
            source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source),
                             path.sourceInfo.adapterId, path.sourceInfo.id};
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                _wcsicmp(source.viewGdiDeviceName, nameW.c_str()) != 0) continue;
            HdrDisplay display{path.targetInfo.adapterId, path.targetInfo.id};
            if (ReadHdrState(display) == ERROR_SUCCESS) {
                enabled = display.enabled;
                return true;
            }
        }
    }
    if (outError) *outError = "cannot read Windows HDR state for the monitor";
    return false;
}

bool ToggleHdr(std::string* outReport) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    LONG status = QueryActivePaths(paths);
    if (status != ERROR_SUCCESS) {
        if (outReport) *outReport = "Cannot query HDR displays (Windows error " + std::to_string(status) + ").";
        return false;
    }

    std::vector<HdrDisplay> displays;
    std::string report;
    bool anyError = false, anyEnabled = false;
    for (const auto& path : paths) {
        const auto& target = path.targetInfo;
        bool duplicate = false;
        for (const auto& display : displays) {
            if (display.id == target.id && display.adapter.LowPart == target.adapterId.LowPart &&
                display.adapter.HighPart == target.adapterId.HighPart) duplicate = true;
        }
        if (duplicate) continue;

        HdrDisplay display{target.adapterId, target.id};
        DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
        name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(name), target.adapterId, target.id};
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS) {
            display.name = ToUtf8(name.monitorFriendlyDeviceName);
        }
        if (display.name.empty()) display.name = "Display " + std::to_string(target.id);

        status = ReadHdrState(display);
        if (status != ERROR_SUCCESS) {
            report += display.name + ": cannot read HDR (Windows error " + std::to_string(status) + ")\n";
            anyError = true;
            continue;
        }
        if (!display.supported) continue;
        anyEnabled = anyEnabled || display.enabled;
        displays.push_back(display);
    }
    if (displays.empty()) {
        if (outReport) *outReport = anyError ? report : "No active display supports HDR.";
        return false;
    }

    const bool enable = !anyEnabled;
    for (auto& display : displays) {
        if (display.enabled != enable) {
            if (enable && display.limitedByPolicy) {
                report += display.name + ": HDR is disabled by Windows policy\n";
                anyError = true;
                continue;
            }
            status = WriteHdrState(display, enable);
            if (status != ERROR_SUCCESS) {
                report += display.name + ": cannot switch HDR (Windows error " + std::to_string(status) + ")\n";
                anyError = true;
                continue;
            }
            // A driver may acknowledge the write before the mode has settled.
            bool verified = false;
            for (int attempt = 0; attempt < 10; ++attempt) {
                if (attempt > 0) Sleep(200); // worker only, never blocks the hook
                if (ReadHdrState(display) == ERROR_SUCCESS && display.enabled == enable) {
                    verified = true;
                    break;
                }
            }
            if (!verified) {
                report += display.name + ": HDR change could not be confirmed\n";
                anyError = true;
                continue;
            }
        }
        report += display.name + (enable ? ": HDR on\n" : ": HDR off\n");
    }
    if (outReport) *outReport = (anyError ? "HDR switch incomplete\n" : "") + report;
    return !anyError;
}

std::vector<MovedWindowState> SnapshotWindows() {
    std::vector<MovedWindowState> out;
    EnumWindows(SnapshotProc, reinterpret_cast<LPARAM>(&out));
    return out;
}

std::vector<MovedWindowState> MoveWindowsToMonitor(const std::vector<MovedWindowState>& snapshot,
                                                    const std::string& gdiDeviceName) {
    std::vector<MovedWindowState> moved;
    if (gdiDeviceName.empty()) return moved;

    // Resolve the GDI name to an HMONITOR by probing a point inside it.
    std::wstring nameW = ToWide(gdiDeviceName);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsExW(nameW.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0)) return moved;

    POINT probe = { dm.dmPosition.x + 1, dm.dmPosition.y + 1 };
    HMONITOR target = MonitorFromPoint(probe, MONITOR_DEFAULTTONULL);
    if (!target) return moved;

    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(target, &info)) return moved;
    const RECT dst = info.rcWork;

    for (const auto& entry : snapshot) {
        if (!IsWindow(entry.hwnd)) continue; // closed since the snapshot was taken

        HMONITOR mon = MonitorFromWindow(entry.hwnd, MONITOR_DEFAULTTONEAREST);
        if (mon == target) continue; // already there

        // The window's CURRENT placement drives the scaling math (correct
        // regardless of any primary-display change since the snapshot, because
        // both this and the source monitor's work area below are read fresh,
        // in the same, current coordinate space) - but what gets recorded as
        // "moved" is the SNAPSHOT's placement, taken before any of this
        // sequence's primary-display change, so a later RestoreWindows() call
        // puts the window back in the coordinate space it will by then have
        // returned to, not the transient one mid-sequence.
        WINDOWPLACEMENT current = {};
        current.length = sizeof(current);
        if (!GetWindowPlacement(entry.hwnd, &current)) continue;

        MONITORINFO srcInfo = {};
        srcInfo.cbSize = sizeof(srcInfo);
        if (!GetMonitorInfoW(mon, &srcInfo)) continue;

        if (ScaleWindowInto(entry.hwnd, current, srcInfo.rcWork, dst)) {
            moved.push_back(entry);
        }
    }
    return moved;
}

void RestoreWindows(const std::vector<MovedWindowState>& moved) {
    for (const auto& w : moved) {
        if (!IsWindow(w.hwnd)) continue; // closed while moved - nothing to restore
        WINDOWPLACEMENT wp = w.original;
        SetWindowPlacement(w.hwnd, &wp);
    }

    // Chromium/Electron apps (observed: Discord, this app's own Claude
    // desktop client) run their own window-bounds logic in response to the
    // WM_DISPLAYCHANGE the primary-monitor swap just fired, and can silently
    // override the SetWindowPlacement above a moment later, landing back on
    // whichever monitor they think is "current" instead of where we just put
    // them. A short wait plus a second identical pass gives our placement the
    // last word in that race, at the cost of ~200ms on the worker thread
    // (harmless here - this only ever runs on the MTA worker, never the hook).
    if (!moved.empty()) {
        Sleep(200);
        for (const auto& w : moved) {
            if (!IsWindow(w.hwnd)) continue;
            WINDOWPLACEMENT wp = w.original;
            SetWindowPlacement(w.hwnd, &wp);
        }
    }
}

bool SetPrimaryMonitor(const std::string& gdiDeviceName, std::string* outError) {
    if (outError) outError->clear();
    if (gdiDeviceName.empty()) {
        if (outError) *outError = "no display selected";
        return false;
    }

    std::wstring targetW = ToWide(gdiDeviceName);

    DEVMODEW targetMode = {};
    targetMode.dmSize = sizeof(targetMode);
    if (!EnumDisplaySettingsExW(targetW.c_str(), ENUM_CURRENT_SETTINGS, &targetMode, 0)) {
        if (outError) *outError = "cannot read the mode of " + gdiDeviceName;
        return false;
    }

    if (targetMode.dmPosition.x == 0 && targetMode.dmPosition.y == 0) {
        return true; // already at the desktop origin = already primary
    }

    // Uses SetDisplayConfig, NOT the legacy ChangeDisplaySettingsEx
    // CDS_SET_PRIMARY + CDS_NORESET batch. That legacy recipe is what most
    // examples show, but it was measured to fail on this machine (dual monitor,
    // NVIDIA): whichever adapter is staged SECOND returns DISP_CHANGE_FAILED,
    // regardless of processing order and regardless of which one carries
    // CDS_SET_PRIMARY, while the final commit still reports success and nothing
    // moves. The reason is that a staged batch necessarily passes through an
    // invalid intermediate state - after the first call either no display sits
    // at (0,0) or two of them do - and this driver validates eagerly instead of
    // deferring to the commit as CDS_NORESET documents. Passing full dmFields
    // instead of DM_POSITION alone does not help (it makes BOTH calls fail).
    //
    // SetDisplayConfig submits the entire path/mode array in one transaction, so
    // no invalid intermediate state exists. Verified working in both directions
    // on this hardware.
    UINT32 pathCount = 0, modeCount = 0;
    LONG status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (status != ERROR_SUCCESS) {
        if (outError) {
            char buf[64];
            sprintf_s(buf, "GetDisplayConfigBufferSizes=%ld", status);
            *outError = buf;
        }
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
    if (status != ERROR_SUCCESS) {
        if (outError) {
            char buf[64];
            sprintf_s(buf, "QueryDisplayConfig=%ld", status);
            *outError = buf;
        }
        return false;
    }
    paths.resize(pathCount);
    modes.resize(modeCount);

    // Find the target's source mode by matching its GDI device name.
    bool found = false;
    POINTL offset = {};
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) continue;
        if (_wcsicmp(sourceName.viewGdiDeviceName, targetW.c_str()) != 0) continue;

        UINT32 idx = path.sourceInfo.modeInfoIdx;
        if (idx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || idx >= modes.size()) continue;
        if (modes[idx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;

        offset = modes[idx].sourceMode.position;
        found = true;
        break;
    }
    if (!found) {
        if (outError) *outError = gdiDeviceName + " is not an active display path";
        return false;
    }
    if (offset.x == 0 && offset.y == 0) return true;

    // Shift every source so the target lands on the desktop origin.
    for (auto& mode : modes) {
        if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        mode.sourceMode.position.x -= offset.x;
        mode.sourceMode.position.y -= offset.y;
    }

    status = SetDisplayConfig((UINT32)paths.size(), paths.data(), (UINT32)modes.size(), modes.data(),
                              SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES);

    // Verify against reality rather than trusting the return code alone.
    DEVMODEW after = {};
    after.dmSize = sizeof(after);
    bool moved = EnumDisplaySettingsExW(targetW.c_str(), ENUM_CURRENT_SETTINGS, &after, 0) &&
                 after.dmPosition.x == 0 && after.dmPosition.y == 0;

    if (status != ERROR_SUCCESS || !moved) {
        if (outError) {
            char buf[96];
            sprintf_s(buf, "SetDisplayConfig=%ld moved=%d", status, moved ? 1 : 0);
            *outError = buf;
        }
        return false;
    }
    return true;
}

} // namespace DisplayControl
