#include <iostream>
#include <string>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Foundation.h>

#pragma comment(lib, "windowsapp")

using namespace winrt::Windows::Media::Control;

int main() {
    winrt::init_apartment();
    try {
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto session = manager.GetCurrentSession();
        if (session) {
            auto info = session.TryGetMediaPropertiesAsync().get();
            std::wcout << L"Title: " << info.Title().c_str() << std::endl;
            std::wcout << L"Artist: " << info.Artist().c_str() << std::endl;
        } else {
            std::cout << "No active session." << std::endl;
        }
    } catch (...) {
        std::cout << "Error" << std::endl;
    }
    return 0;
}
