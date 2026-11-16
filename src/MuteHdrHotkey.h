#pragma once
#include "HdrHotkey.h"
#include <array>

// Hook-thread-only state. HDR shortcuts use exact modifiers so the two
// defaults (Mute+H / Win+Mute+H) cannot fire together. Mute playback waits
// for release; all consumed key-ups/repeats stay consumed after rebinding.
class MuteHdrHotkey {
public:
    enum class Action { PassThrough, Consume, BeginMute, PlayPause, ToggleHdr, ToggleHdrEffect, ReplayKey };
    struct Result {
        Action action = Action::PassThrough;
        WORD replayKey = 0;
        WORD forwardKey = 0; // original event to replay AFTER replayKey down/up
        bool forwardDown = false;
        bool maskMenu = false;
    };

    void SuppressUntilRelease(DWORD key) {
        if (key < m_consumed.size()) m_consumed[key] = true;
    }

    Result Process(DWORD key, bool down, bool capturing,
                   HdrHotkey windows = HdrHotkey::WindowsDefault(),
                   HdrHotkey monitor = HdrHotkey::MonitorDefault()) {
        if (key >= m_down.size()) return {};
        const bool repeated = m_down[key];
        m_down[key] = down;
        if (key == VK_VOLUME_MUTE && down && !repeated) m_muteUsed = capturing;
        if (capturing && m_down[VK_VOLUME_MUTE]) m_muteUsed = true;

        // A partial Ctrl/Shift/Alt/Win + key chord may still acquire Mute.
        // If it does not, preserve the original shortcut on release. Unrelated
        // input flushes it first, in its original modifier state.
        if (m_deferredKey && key != VK_VOLUME_MUTE &&
            (key != m_deferredKey || !down)) {
            const WORD pending = m_deferredKey;
            m_deferredKey = 0;
            if (!down) m_consumed[key] = false;
            return {Action::ReplayKey, pending,
                    static_cast<WORD>(key == pending ? 0 : key), down};
        }

        if (!down) {
            if (key == VK_VOLUME_MUTE) {
                return {repeated && !m_muteUsed && !capturing ? Action::PlayPause : Action::Consume};
            }
            const bool consumed = m_consumed[key];
            m_consumed[key] = false;
            return {consumed ? Action::Consume : Action::PassThrough};
        }
        if (m_consumed[key] || (key == VK_VOLUME_MUTE && repeated)) return {Action::Consume};

        const bool conflict = windows.Enabled() && windows == monitor;
        const unsigned mods = Modifiers();
        const HdrHotkey bindings[] = {windows, monitor};
        if (!capturing && !conflict && !repeated) {
            for (int i = 0; i < 2; ++i) {
                const auto& binding = bindings[i];
                if (!binding.Enabled() || mods != binding.modifiers) continue;
                const bool needsMute = (binding.modifiers & HdrHotkey::Mute) != 0;
                if (needsMute && m_muteUsed) continue;
                if (key != static_cast<DWORD>(binding.key) &&
                    !(key == VK_VOLUME_MUTE && m_deferredKey == binding.key)) continue;
                if (needsMute) m_muteUsed = true;
                m_consumed[binding.key] = true;
                m_deferredKey = 0;
                return {i == 0 ? Action::ToggleHdr : Action::ToggleHdrEffect, 0, 0, false,
                        (mods & (HdrHotkey::Win | HdrHotkey::Alt)) != 0};
            }
            // Only defer modified keys. Ordinary letters retain normal typing;
            // for an unmodified Mute+letter shortcut, hold Mute first.
            for (const auto& binding : bindings) {
                if (binding.Enabled() && key == static_cast<DWORD>(binding.key) &&
                    (binding.modifiers & HdrHotkey::Mute) && mods != 0 &&
                    mods == (binding.modifiers & ~HdrHotkey::Mute)) {
                    m_deferredKey = static_cast<WORD>(key);
                    m_consumed[key] = true;
                    return {Action::Consume};
                }
            }
        }
        if (key == VK_VOLUME_MUTE) return {Action::BeginMute};
        // Further presses of a shortcut key within the same Mute hold must not
        // leak text or run a second toggle.
        if (!capturing && m_muteUsed && m_down[VK_VOLUME_MUTE]) {
            for (const auto& binding : bindings) {
                if (binding.Enabled() && key == static_cast<DWORD>(binding.key) &&
                    (binding.modifiers & HdrHotkey::Mute)) {
                    m_consumed[key] = true;
                    return {Action::Consume};
                }
            }
        }
        return {};
    }

private:
    unsigned Modifiers() const {
        unsigned mods = 0;
        if (m_down[VK_CONTROL] || m_down[VK_LCONTROL] || m_down[VK_RCONTROL]) mods |= HdrHotkey::Ctrl;
        if (m_down[VK_SHIFT] || m_down[VK_LSHIFT] || m_down[VK_RSHIFT]) mods |= HdrHotkey::Shift;
        if (m_down[VK_MENU] || m_down[VK_LMENU]) mods |= HdrHotkey::Alt;
        if (m_down[VK_LWIN] || m_down[VK_RWIN]) mods |= HdrHotkey::Win;
        if (m_down[VK_VOLUME_MUTE]) mods |= HdrHotkey::Mute;
        if (m_down[VK_RMENU]) mods |= 32u; // AltGr never matches Left Alt or a plain shortcut
        return mods;
    }
    std::array<bool, 256> m_down{};
    std::array<bool, 256> m_consumed{};
    WORD m_deferredKey = 0;
    bool m_muteUsed = false;
};
