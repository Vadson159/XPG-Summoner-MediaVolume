#include "Autostart.h"
#include <cstdio>
#include <cstdlib>

static void Expect(const char* name, bool condition) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        std::exit(1);
    }
}

int main() {
    Expect("FormatRunCommand wraps path in quotes",
           Autostart::FormatRunCommand(L"S:\\_Vibe_Coding\\SmartVolumeMixer.exe") ==
           L"\"S:\\_Vibe_Coding\\SmartVolumeMixer.exe\"");
    Expect("FormatRunCommand empty stays empty",
           Autostart::FormatRunCommand(L"").empty());

    Expect("ExtractCommandPath quoted path",
           Autostart::ExtractCommandPath(L"\"S:\\My Apps\\SmartVolumeMixer.exe\"") ==
           L"S:\\My Apps\\SmartVolumeMixer.exe");
    Expect("ExtractCommandPath quoted path with args and surrounding spaces",
           Autostart::ExtractCommandPath(L"  \"S:\\My Apps\\SmartVolumeMixer.exe\" --minimized  ") ==
           L"S:\\My Apps\\SmartVolumeMixer.exe");
    Expect("ExtractCommandPath unquoted path with spaces",
           Autostart::ExtractCommandPath(L"S:\\Program Files\\SmartVolumeMixer.exe") ==
           L"S:\\Program Files\\SmartVolumeMixer.exe");
    Expect("ExtractCommandPath unquoted path with spaces and args",
           Autostart::ExtractCommandPath(L"S:\\Program Files\\SmartVolumeMixer.exe --silent") ==
           L"S:\\Program Files\\SmartVolumeMixer.exe");
    Expect("ExtractCommandPath whitespace only is empty",
           Autostart::ExtractCommandPath(L"   \t  ").empty());

    Expect("PathsEqual ignores case",
           Autostart::PathsEqual(L"S:\\_Vibe_Coding\\Build\\Release\\SmartVolumeMixer.exe",
                                 L"s:\\_vibe_coding\\build\\release\\smartvolumemixer.exe"));
    Expect("PathsEqual normalizes slashes and extended prefix",
           Autostart::PathsEqual(L"\\\\?\\S:/_Vibe_Coding/build/Release/SmartVolumeMixer.exe",
                                 L"S:\\_Vibe_Coding\\build\\Release\\SmartVolumeMixer.exe"));
    Expect("PathsEqual distinguishes different builds",
           !Autostart::PathsEqual(L"S:\\_Vibe_Coding\\build\\Release\\SmartVolumeMixer.exe",
                                  L"S:\\_Vibe_Coding\\build-opt\\Release\\SmartVolumeMixer.exe"));
    Expect("PathsEqual rejects empty",
           !Autostart::PathsEqual(L"", L""));

    const unsigned char enabled2[] = { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const unsigned char enabled6[] = { 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const unsigned char disabled1[] = { 0x01, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const unsigned char disabled3[] = { 0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    Expect("StartupApproved empty/null counts as enabled",
           Autostart::IsStartupApprovedBytes(nullptr, 0));
    Expect("StartupApproved 0x02 is enabled",
           Autostart::IsStartupApprovedBytes(enabled2, sizeof(enabled2)));
    Expect("StartupApproved 0x06 is enabled",
           Autostart::IsStartupApprovedBytes(enabled6, sizeof(enabled6)));
    Expect("StartupApproved 0x01 is disabled",
           !Autostart::IsStartupApprovedBytes(disabled1, sizeof(disabled1)));
    Expect("StartupApproved 0x03 is disabled",
           !Autostart::IsStartupApprovedBytes(disabled3, sizeof(disabled3)));

    std::printf("AutostartTests: all checks passed\n");
    return 0;
}
