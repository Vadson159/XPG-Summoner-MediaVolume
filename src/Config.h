#pragma once
#include <vector>
#include <string>
#include <windows.h>

struct AppConfig {
    int modifierKey = 0; // 0 = no modifier
    int menuKey = VK_F8;
    bool menuCtrl = false;
    bool menuShift = false;
    bool menuAlt = false;
    
    int infoKey = VK_F9;
    bool infoCtrl = false;
    bool infoShift = false;
    bool infoAlt = false;

    float volStep = 0.03f;
    float volStepLarge = 0.10f;

    int volDebounceThreshold = 2;   // Min ticks before triggering (1 = no debounce)
    int volDebounceWindowMs = 200;  // Time window to accumulate ticks (ms)

    std::vector<std::string> mediaApps;

    // Customization
    int osdOffsetX = 50;
    int osdOffsetY = 100;
    float barColor[4] = { 0.4f, 0.6f, 1.0f, 1.0f };
    int fontSizeNormal = 20;
    int fontSizeLarge = 26;
    int fontSizeSmall = 16;
    std::string fontFamily = "Segoe UI";


    void Load();
    void Save();
};

extern AppConfig g_Config;
