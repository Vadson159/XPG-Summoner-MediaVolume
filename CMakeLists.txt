cmake_minimum_required(VERSION 3.20)
project(SmartVolumeMixer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
target_link_libraries(imgui PUBLIC d3d11 d3dcompiler dwmapi)

add_executable(SmartVolumeMixer WIN32
    src/main.cpp
    src/OverlayWindow.cpp src/OverlayWindow.h
    src/AudioManager.cpp src/AudioManager.h
    src/InputHook.cpp src/InputHook.h
    src/Config.cpp src/Config.h
    src/UIManager.cpp src/UIManager.h
    src/resources.rc
)

target_link_libraries(SmartVolumeMixer PRIVATE
    imgui
    nlohmann_json::nlohmann_json
    d3d11
    d3dcompiler
    dwmapi
    ole32
    oleaut32
    psapi
)

target_compile_definitions(SmartVolumeMixer PRIVATE UNICODE _UNICODE)
