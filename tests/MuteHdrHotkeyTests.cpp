#include "MuteHdrHotkey.h"
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

using Action = MuteHdrHotkey::Action;
struct Step { DWORD key; bool down; Action expected; bool capturing = false; };

static void Check(const char* name, std::initializer_list<Step> steps, HdrHotkey windows = HdrHotkey::WindowsDefault(), HdrHotkey monitor = HdrHotkey::MonitorDefault()) {
    MuteHdrHotkey hotkey;
    int index = 0;
    for (const auto& step : steps) {
        if (hotkey.Process(step.key, step.down, step.capturing, windows, monitor).action != step.expected) {
            std::fprintf(stderr, "FAIL: %s, step %d\n", name, index);
            std::exit(1);
        }
        ++index;
    }
}

int main() {
    constexpr DWORD mute = VK_VOLUME_MUTE;
    Check("tap waits for release; repeat cannot double-play", {
        {mute, true, Action::BeginMute}, {mute, true, Action::Consume},
        {mute, false, Action::PlayPause}, {mute, false, Action::Consume},
        {mute, true, Action::BeginMute}, {mute, false, Action::PlayPause}});
    Check("HDR, H released first, no playback", {
        {mute, true, Action::BeginMute}, {'H', true, Action::ToggleHdrEffect},
        {'H', false, Action::Consume}, {mute, false, Action::Consume}});
    Check("HDR, mute released first; H repeats and release stay swallowed", {
        {mute, true, Action::BeginMute}, {'H', true, Action::ToggleHdrEffect},
        {mute, false, Action::Consume}, {'H', true, Action::Consume},
        {'H', false, Action::Consume}, {'H', true, Action::PassThrough},
        {'H', false, Action::PassThrough}});
    Check("one toggle per hold, next chord works", {
        {mute, true, Action::BeginMute}, {'H', true, Action::ToggleHdrEffect},
        {mute, true, Action::Consume}, {'H', true, Action::Consume},
        {'H', false, Action::Consume}, {'H', true, Action::Consume},
        {'H', false, Action::Consume}, {mute, false, Action::Consume},
        {mute, true, Action::BeginMute}, {'H', true, Action::ToggleHdrEffect},
        {'H', false, Action::Consume}, {mute, false, Action::Consume}});
    Check("H alone and other keys pass through", {
        {'H', true, Action::PassThrough}, {'H', true, Action::PassThrough},
        {'H', false, Action::PassThrough}, {'X', true, Action::PassThrough},
        {'X', false, Action::PassThrough}});
    Check("pre-held H cannot accidentally trigger HDR or lose its release", {
        {'H', true, Action::PassThrough}, {mute, true, Action::BeginMute},
        {'H', true, Action::PassThrough}, {'H', false, Action::PassThrough},
        {mute, false, Action::PlayPause}});
    Check("key capture takes priority", {
        {mute, true, Action::BeginMute, true}, {'H', true, Action::PassThrough, true},
        {'H', true, Action::PassThrough, true}, {'H', false, Action::PassThrough, true},
        {mute, false, Action::Consume}});
    Check("other keys do not discard the pending playback", {
        {mute, true, Action::BeginMute}, {'X', true, Action::PassThrough},
        {'X', false, Action::PassThrough}, {mute, false, Action::PlayPause}});
    Check("Win + H then Mute toggles Windows HDR, never the monitor effect or playback", {
        {VK_LWIN, true, Action::PassThrough}, {'H', true, Action::Consume},
        {mute, true, Action::ToggleHdr}, {mute, true, Action::Consume},
        {'H', true, Action::Consume}, {mute, false, Action::Consume},
        {'H', false, Action::Consume}, {VK_LWIN, false, Action::PassThrough}});
    Check("Win + Mute then H also toggles Windows HDR", {
        {VK_LWIN, true, Action::PassThrough}, {mute, true, Action::BeginMute},
        {'H', true, Action::ToggleHdr}, {'H', false, Action::Consume},
        {mute, false, Action::Consume}, {VK_LWIN, false, Action::PassThrough}});
    Check("right Windows key and Win released first", {
        {VK_RWIN, true, Action::PassThrough}, {'H', true, Action::Consume},
        {mute, true, Action::ToggleHdr}, {VK_RWIN, false, Action::PassThrough},
        {'H', true, Action::Consume}, {'H', false, Action::Consume},
        {mute, false, Action::Consume}});
    Check("Win+H alone still works, H released first", {
        {VK_LWIN, true, Action::PassThrough}, {'H', true, Action::Consume},
        {'H', true, Action::Consume}, {'H', false, Action::ReplayKey},
        {VK_LWIN, false, Action::PassThrough}});
    Check("Win+H alone still works, Win released first", {
        {VK_LWIN, true, Action::PassThrough}, {'H', true, Action::Consume},
        {VK_LWIN, false, Action::ReplayKey}, {'H', true, Action::Consume},
        {'H', false, Action::Consume}, {'H', true, Action::PassThrough},
        {'H', false, Action::PassThrough}});
    Check("two Windows keys, one released before the chord", {
        {VK_LWIN, true, Action::PassThrough}, {VK_RWIN, true, Action::PassThrough},
        {VK_LWIN, false, Action::PassThrough}, {mute, true, Action::BeginMute},
        {'H', true, Action::ToggleHdr}, {'H', false, Action::Consume},
        {mute, false, Action::Consume}, {VK_RWIN, false, Action::PassThrough}});
    Check("capture suppresses monitor action", {
        {VK_LWIN, true, Action::PassThrough, true}, {'H', true, Action::PassThrough, true},
        {mute, true, Action::BeginMute, true}, {'H', false, Action::PassThrough, true},
        {mute, false, Action::Consume, true}, {VK_LWIN, false, Action::PassThrough, true}});
    Check("ordinary Windows shortcuts pass through", {
        {VK_LWIN, true, Action::PassThrough}, {'R', true, Action::PassThrough},
        {'R', false, Action::PassThrough}, {VK_LWIN, false, Action::PassThrough}});
    Check("Win chord does not contaminate the next plain Mute+H", {
        {VK_LWIN, true, Action::PassThrough}, {'H', true, Action::Consume},
        {mute, true, Action::ToggleHdr}, {'H', false, Action::Consume},
        {mute, false, Action::Consume}, {VK_LWIN, false, Action::PassThrough},
        {mute, true, Action::BeginMute}, {'H', true, Action::ToggleHdrEffect},
        {'H', false, Action::Consume}, {mute, false, Action::Consume}});
    const HdrHotkey disabled{0, 0};
    Check("custom main key, old default passes through", {
        {mute, true, Action::BeginMute}, {'H', true, Action::PassThrough},
        {'H', false, Action::PassThrough}, {'J', true, Action::ToggleHdrEffect},
        {'J', false, Action::Consume}, {mute, false, Action::Consume}}, disabled, {'J', HdrHotkey::Mute});
    Check("shortcut without Mute, no autorepeat", {
        {VK_F10, true, Action::ToggleHdr}, {VK_F10, true, Action::Consume},
        {VK_F10, false, Action::Consume}, {VK_F10, true, Action::ToggleHdr},
        {VK_F10, false, Action::Consume}}, {VK_F10, 0}, disabled);
    Check("Ctrl+LeftAlt+K without Mute", {
        {VK_LCONTROL, true, Action::PassThrough}, {VK_LMENU, true, Action::PassThrough},
        {'K', true, Action::ToggleHdr}, {'K', false, Action::Consume},
        {VK_LMENU, false, Action::PassThrough}, {VK_LCONTROL, false, Action::PassThrough}},
        {'K', HdrHotkey::Ctrl | HdrHotkey::Alt}, disabled);
    Check("exact modifiers reject extras", {
        {VK_LSHIFT, true, Action::PassThrough}, {mute, true, Action::BeginMute},
        {'H', true, Action::PassThrough}, {'H', false, Action::PassThrough},
        {mute, false, Action::PlayPause}, {VK_LSHIFT, false, Action::PassThrough}});
    Check("disabled shortcuts pass keys and retain Mute playback", {
        {VK_LWIN, true, Action::PassThrough}, {'H', true, Action::PassThrough},
        {'H', false, Action::PassThrough}, {VK_LWIN, false, Action::PassThrough},
        {mute, true, Action::BeginMute}, {'H', true, Action::PassThrough},
        {'H', false, Action::PassThrough}, {mute, false, Action::PlayPause}}, disabled, disabled);
    Check("duplicate bindings do not toggle either HDR", {
        {mute, true, Action::BeginMute}, {'H', true, Action::PassThrough},
        {'H', false, Action::PassThrough}, {mute, false, Action::PlayPause}},
        HdrHotkey::MonitorDefault(), HdrHotkey::MonitorDefault());
    Check("right Ctrl and Shift work with a remapped key", {
        {VK_RCONTROL, true, Action::PassThrough}, {VK_RSHIFT, true, Action::PassThrough},
        {'J', true, Action::Consume}, {mute, true, Action::ToggleHdrEffect},
        {'J', false, Action::Consume}, {mute, false, Action::Consume},
        {VK_RCONTROL, false, Action::PassThrough}, {VK_RSHIFT, false, Action::PassThrough}},
        disabled, {'J', HdrHotkey::Ctrl | HdrHotkey::Shift | HdrHotkey::Mute});
    Check("AltGr never matches Left Alt", {
        {VK_LCONTROL, true, Action::PassThrough}, {VK_RMENU, true, Action::PassThrough},
        {'K', true, Action::PassThrough}, {'K', false, Action::PassThrough},
        {VK_RMENU, false, Action::PassThrough}, {VK_LCONTROL, false, Action::PassThrough}},
        {'K', HdrHotkey::Ctrl | HdrHotkey::Alt}, disabled);
    Check("capture bypasses non-Mute HDR shortcuts", {
        {VK_F10, true, Action::PassThrough, true}, {VK_F10, false, Action::PassThrough, true}},
        {VK_F10, 0}, disabled);

    // Check replay payload and rebinding while a swallowed key is still held.
    MuteHdrHotkey hotkey;
    hotkey.Process(VK_LWIN, true, false);
    hotkey.Process('H', true, false);
    auto replay = hotkey.Process(VK_LWIN, false, false);
    if (replay.action != Action::ReplayKey || replay.replayKey != 'H' ||
        replay.forwardKey != VK_LWIN || replay.forwardDown) return 1;
    if (hotkey.Process('H', false, false, disabled, disabled).action != Action::Consume) return 1;
    hotkey = {};
    hotkey.Process(VK_VOLUME_MUTE, true, false);
    if (hotkey.Process('H', true, false).action != Action::ToggleHdrEffect) return 1;
    if (hotkey.Process('H', false, false, disabled, disabled).action != Action::Consume) return 1;
    if (hotkey.Process(VK_VOLUME_MUTE, false, false, disabled, disabled).action != Action::Consume) return 1;
    hotkey = {};
    hotkey.Process(VK_LWIN, true, false);
    hotkey.Process(VK_VOLUME_MUTE, true, false);
    if (!hotkey.Process('H', true, false).maskMenu) return 1;
    if (HdrHotkey::Unpack(HdrHotkey::WindowsDefault().Pack()) != HdrHotkey::WindowsDefault()) return 1;
    std::puts("PASS: 26 keyboard sequences, replay payload, live rebinding and menu masking");
}

