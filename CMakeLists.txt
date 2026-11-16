cmake_minimum_required(VERSION 3.20)
project(SmartVolumeMixer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Whole-program optimisation for the shipping build (/GL + /LTCG). Set before
# any target is created so both this exe and the imgui static lib pick it up -
# the payoff is cross-module inlining of the ImGui draw path and a noticeably
# smaller binary. Debug builds stay unaffected so they keep linking fast.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

if(MSVC)
    # /Gy + /OPT:REF,ICF: fold identical functions and drop unreferenced ones.
    # /Gw: the same for global data (ImGui and nlohmann_json carry plenty).
    # /Oi: intrinsics instead of calls for memcpy/strlen and friends.
    add_compile_options($<$<CONFIG:Release>:/Gy> $<$<CONFIG:Release>:/Gw> $<$<CONFIG:Release>:/Oi>)
    add_link_options($<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

include(FetchContent)

# nlohmann_json
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# imgui
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4
)
FetchContent_MakeAvailable(imgui)

# Add ImGui as a library
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
# PUBLIC because an ImGui config define has to be identical in every
# translation unit that sees imgui.h, library and app alike. The metrics /
# debug-log / item-picker windows are dead weight in a shipping overlay.
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEBUG_TOOLS)
# Only imgui_impl_win32.cpp reads this. Without it the backend LoadLibrary()s
# xinput1_4.dll (which drags SETUPAPI in with it) and polls a gamepad nothing
# here ever reads.
target_compile_definitions(imgui PRIVATE IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)
# No d3dcompiler: src/ImGuiDx11Shaders.cpp answers the backend's D3DCompile()
# calls with precompiled bytecode, so d3dcompiler_47.dll is never loaded.
target_link_libraries(imgui PUBLIC d3d11 dwmapi)

add_executable(SmartVolumeMixer WIN32
    src/main.cpp
    src/OverlayWindow.cpp src/OverlayWindow.h
    src/ImGuiDx11Shaders.cpp
    src/AudioManager.cpp src/AudioManager.h
    src/AudioDevices.cpp src/AudioDevices.h
    src/DisplayControl.cpp src/DisplayControl.h
    src/AmdDdcSideChannel.cpp src/AmdDdcSideChannel.h src/AmdAdl.h
    src/QuickActions.cpp src/QuickActions.h
    src/InputHook.cpp src/InputHook.h src/MuteHdrHotkey.h src/HdrHotkey.h
    src/Config.cpp src/Config.h
    src/UIManager.cpp src/UIManager.h
    src/PolicyConfig.h
    src/StrUtil.h
    src/Perf.h
    src/resources.rc
)

target_link_libraries(SmartVolumeMixer PRIVATE
    imgui
    nlohmann_json::nlohmann_json
    d3d11
    dwmapi
    ole32
    oleaut32
    psapi
    dxva2      # DDC/CI monitor configuration (SetVCPFeature)
    user32
    windowsapp # C++/WinRT (SMTC); also pinned by a pragma in AudioManager.cpp
)

target_compile_definitions(SmartVolumeMixer PRIVATE UNICODE _UNICODE)

# Delay-loaded: d3d11.dll is only needed once something is drawn (the device
# is created by the first frame, see OverlayWindow::Create), so a session that
# never shows the OSD never maps it - nor dxgi/dxcore and the driver stack
# behind it. Never FreeLibrary'd once loaded.
if(MSVC)
    target_link_options(SmartVolumeMixer PRIVATE "/DELAYLOAD:d3d11.dll")
    target_link_libraries(SmartVolumeMixer PRIVATE delayimp)
endif()

# Measurement build for the optimization work (src/Perf.h, tools/perf/).
# Writes timings to perf.log next to the exe. Never ship with this on.
option(SVM_PERF "Build with the perf.log timing harness" OFF)
if(SVM_PERF)
    target_compile_definitions(SmartVolumeMixer PRIVATE SVM_PERF)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(MuteHdrHotkeyTests tests/MuteHdrHotkeyTests.cpp)
    target_include_directories(MuteHdrHotkeyTests PRIVATE src)
    add_test(NAME MuteHdrHotkeyTests COMMAND MuteHdrHotkeyTests)
endif()
