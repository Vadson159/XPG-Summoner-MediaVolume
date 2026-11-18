#pragma once
#include <string>

// Some LG monitors silently ignore the standard MCCS "Input Source" VCP
// command (0x60) sent over the normal DDC/CI channel — the write is
// acknowledged (ACK'd on the I2C bus) but the monitor's firmware never acts on
// it. They instead expose an undocumented "service" side-channel: the same
// "Set VCP Feature" packet shape, but with the DDC/CI source-address byte
// changed from 0x51 to 0x50 and the feature code changed from 0x60 to 0xF4.
// Verified working end-to-end on an LG UltraGear 27GS85Q-B via the real
// hotkey: 0xD0 -> DisplayPort, 0x91 -> HDMI-2.
//
// The standard Windows dxva2.dll DDC/CI API (what DisplayControl.cpp uses)
// has no parameter to override that source-address byte, so this side channel
// is only reachable through a GPU vendor's own raw-I2C API. This module uses
// AMD's ADL (github.com/GPUOpen-LibrariesAndSDKs/display-library, MIT), which
// this app depends on already having installed since it targets an AMD GPU
// machine. On any other GPU vendor, or if ADL is unavailable, every call
// below simply fails with an explanatory error — there is no other fallback.
//
// The monitor does not confirm success on this channel (matches the
// community's own tooling, which requires a --noverify-style flag for it), so
// SetInput() can only report whether the underlying ADL call succeeded, not
// whether the monitor actually reacted.
namespace AmdDdcSideChannel {

bool IsAvailable();

// nameMatch: case-insensitive substring of the ADL display name (e.g. the
// same swapMonitorMatch the app already uses for the standard DDC/CI path).
bool SetInput(const std::string& nameMatch, int value, std::string* outError);

} // namespace AmdDdcSideChannel
