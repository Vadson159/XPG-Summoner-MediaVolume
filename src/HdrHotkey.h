#pragma once
#include <windows.h>
#include <cstdint>

struct HdrHotkey {
    enum Modifier : unsigned { Ctrl = 1, Shift = 2, Alt = 4, Win = 8, Mute = 16 };
    int key = 'H'; // 0 disables the shortcut
    unsigned modifiers = Mute;

    bool operator==(const HdrHotkey&) const = default;
    static bool IsPrimaryKey(int key) {
        return key > VK_XBUTTON2 && key < 256 &&
            key != VK_SHIFT && key != VK_CONTROL && key != VK_MENU &&
            key != VK_LWIN && key != VK_RWIN &&
            !(key >= VK_LSHIFT && key <= VK_RMENU) &&
            !(key >= VK_VOLUME_MUTE && key <= VK_MEDIA_PLAY_PAUSE);
    }
    bool Enabled() const { return IsPrimaryKey(key) && (modifiers & ~31u) == 0; }
    uint32_t Pack() const { return (static_cast<uint32_t>(key) & 255u) | ((modifiers & 31u) << 8); }
    static HdrHotkey Unpack(uint32_t value) { return {static_cast<int>(value & 255u), (value >> 8) & 31u}; }
    static HdrHotkey WindowsDefault() { return {'H', Win | Mute}; }
    static HdrHotkey MonitorDefault() { return {'H', Mute}; }
};
