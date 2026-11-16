#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct MonitorInfo {
    std::string deviceName;   // GDI name, e.g. "\\\\.\\DISPLAY1"
    std::string deviceId;     // PnP id, e.g. "MONITOR\\GSM7A2B\\{4d36e96e-...}\\0002"
    std::string description;  // monitor name reported by the driver / DDC
    bool isPrimary = false;
    int width = 0;
    int height = 0;
    int refreshHz = 0;
    bool ddcOk = false;       // DDC/CI answered, so input switching should work
    int currentInput = -1;    // VCP 0x60 current value, -1 = unknown
    int maxInput = -1;        // VCP 0x60 maximum value, -1 = unknown
};

// Monitor input switching (DDC/CI, MCCS VCP code 0x60) plus primary-display
// selection.
//
// DDC/CI round-trips talk to the monitor over the video cable and can take
// hundreds of milliseconds each, so nothing here may run on the UI thread —
// the low-level keyboard hook lives there and Windows silently unhooks it if it
// stalls. Everything is driven from the MTA worker thread instead.
namespace DisplayControl {

// queryInputs=false skips all DDC/CI traffic and returns just the GDI-level
// facts (fast).
std::vector<MonitorInfo> ListMonitors(bool queryInputs);

// Case-insensitive substring test against the device id, description and GDI
// name. An empty match never matches (avoids accidentally driving every screen).
bool Matches(const MonitorInfo& monitor, const std::string& match);

bool SetMonitorInput(const std::string& match, int vcpValue, std::string* outError);

// Worker only. If any active HDR display is enabled, turn HDR off; otherwise
// turn it on for all capable displays. Reads Windows state on each invocation
// and verifies writes. Reports unsupported displays/errors through outReport.
bool ToggleHdr(std::string* outReport);

// LG picture-mode VCP 0x15. Call only for a model with verified mappings.
bool GetMonitorPictureMode(const std::string& match, int& outMode, std::string* outError);
bool SetMonitorPictureMode(const std::string& match, int mode, std::string* outError);
bool GetMonitorHdrEnabled(const std::string& gdiName, bool& enabled, std::string* outError);

// Makes the given GDI display (e.g. "\\\\.\\DISPLAY2") the primary one,
// shifting every other display so the desktop origin follows it.
bool SetPrimaryMonitor(const std::string& gdiDeviceName, std::string* outError);

// A window's placement, either as a snapshot (SnapshotWindows) or the
// pre-move placement of a window MoveWindowsToMonitor actually relocated.
struct MovedWindowState {
    HWND hwnd = nullptr;
    WINDOWPLACEMENT original = {};
};

// Snapshots every ordinary top-level window's CURRENT placement, in the
// CURRENT desktop coordinate space. Call this BEFORE SetPrimaryMonitor: the
// desktop's coordinate origin shifts when the primary display changes, so a
// placement captured after that shift is in the wrong space to hand to
// RestoreWindows() later, once the primary has changed again and the origin
// has shifted back.
std::vector<MovedWindowState> SnapshotWindows();

// Relocates every window in `snapshot` that is not already on the given
// display onto it, scaling each window's position/size proportionally into
// the target's work area (using each window's CURRENT position for the
// scaling math — safe to call after SetPrimaryMonitor). Maximized and
// minimized windows keep their state. Changing the primary display does NOT
// move existing windows by itself, which is why this exists. Returns each
// window actually moved, carrying its `snapshot` (pre-sequence) placement,
// for a later RestoreWindows() call.
//
// Deliberately NOT symmetric with RestoreWindows: this moves everything in
// the snapshot that isn't already on the target (so console mode picks up
// windows regardless of which monitor they started on), while RestoreWindows
// only touches the windows it is explicitly given — the two together are what
// let a PC->console->PC round trip put each window back where the USER had
// it, instead of collapsing windows that were already on the second monitor
// onto the first one too.
std::vector<MovedWindowState> MoveWindowsToMonitor(const std::vector<MovedWindowState>& snapshot,
                                                    const std::string& gdiDeviceName);

// Restores each window to the exact placement captured by MoveWindowsToMonitor
// (or SnapshotWindows). Windows closed in the meantime are silently skipped.
void RestoreWindows(const std::vector<MovedWindowState>& moved);

} // namespace DisplayControl
