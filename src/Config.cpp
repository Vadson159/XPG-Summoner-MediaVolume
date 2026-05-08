#include "Config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

#include <filesystem>

using json = nlohmann::json;
namespace fs = std::filesystem;

AppConfig g_Config;

static std::string GetConfigPath() {
    char szPath[MAX_PATH];
    GetModuleFileNameA(NULL, szPath, MAX_PATH);
    return (fs::path(szPath).parent_path() / "config.json").string();
}


void AppConfig::Load() {
    std::ifstream file(GetConfigPath());
    if (!file.is_open()) {
        mediaApps = {
            "spotify.exe", "msedge.exe", "qobuz.exe", "chrome.exe",
            "firefox.exe", "brave.exe", "opera.exe", "opera_gx.exe",
            "vlc.exe", "foobar2000.exe", "aimp.exe", "wmplayer.exe",
            "music.ui.exe", "applemusicwin.exe", "tidal.exe", "mpc-hc64.exe",
            "mpc-be64.exe", "mpc-hc.exe", "mpc-be.exe", "yandex.exe", "arc.exe"
        };
        modifierKey = 0;
        menuKey = VK_F8;
        menuCtrl = false;
        menuShift = false;
        menuAlt = false;
        infoKey = VK_F9;
        infoCtrl = false;
        infoShift = false;
        infoAlt = false;
        volStep = 0.03f;
        volStepLarge = 0.10f;
        volDebounceThreshold = 2;
        volDebounceWindowMs = 200;
        Save();
        return;
    }

    try {
        json j;
        file >> j;
        modifierKey = j.value("modifierKey", 0);
        menuKey = j.value("menuKey", VK_F8);
        menuCtrl = j.value("menuCtrl", false);
        menuShift = j.value("menuShift", false);
        menuAlt = j.value("menuAlt", false);
        infoKey = j.value("infoKey", (int)VK_F9);
        infoCtrl = j.value("infoCtrl", false);
        infoShift = j.value("infoShift", false);
        infoAlt = j.value("infoAlt", false);
        volStep = j.value("volStep", 0.03f);
        volStepLarge = j.value("volStepLarge", 0.10f);
        volDebounceThreshold = j.value("volDebounceThreshold", 2);
        volDebounceWindowMs = j.value("volDebounceWindowMs", 200);
        mediaApps = j.value("mediaApps", std::vector<std::string>{});

        osdOffsetX = j.value("osdOffsetX", 50);
        osdOffsetY = j.value("osdOffsetY", 100);
        fontSizeNormal = j.value("fontSizeNormal", 20);
        fontSizeLarge = j.value("fontSizeLarge", 26);
        fontSizeSmall = j.value("fontSizeSmall", 16);
        fontFamily = j.value("fontFamily", "Segoe UI");
        
        if (j.contains("barColor") && j["barColor"].is_array() && j["barColor"].size() == 4) {
            for(int i=0; i<4; i++) barColor[i] = j["barColor"][i];
        }
    } catch (...) {
        // Fallback
    }
}

void AppConfig::Save() {
    json j;
    j["modifierKey"] = modifierKey;
    j["menuKey"] = menuKey;
    j["menuCtrl"] = menuCtrl;
    j["menuShift"] = menuShift;
    j["menuAlt"] = menuAlt;
    j["infoKey"] = infoKey;
    j["infoCtrl"] = infoCtrl;
    j["infoShift"] = infoShift;
    j["infoAlt"] = infoAlt;
    j["volStep"] = volStep;
    j["volStepLarge"] = volStepLarge;
    j["volDebounceThreshold"] = volDebounceThreshold;
    j["volDebounceWindowMs"] = volDebounceWindowMs;
    j["mediaApps"] = mediaApps;
    j["osdOffsetX"] = osdOffsetX;
    j["osdOffsetY"] = osdOffsetY;
    j["fontSizeNormal"] = fontSizeNormal;
    j["fontSizeLarge"] = fontSizeLarge;
    j["fontSizeSmall"] = fontSizeSmall;
    j["fontFamily"] = fontFamily;
    j["barColor"] = { barColor[0], barColor[1], barColor[2], barColor[3] };

    std::ofstream file(GetConfigPath());
    file << j.dump(4);
}
