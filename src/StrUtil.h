#pragma once
#include <windows.h>
#include <string>
#include <string_view>

// Tiny UTF-8 <-> UTF-16 helpers. The app stores every user-visible string in
// config.json as UTF-8 but the Win32/COM APIs it drives are all wide, so the
// conversion happens on every device/monitor lookup. Takes a view so a
// winrt::hstring (SMTC AUMIDs, track titles) converts without an intermediate
// std::wstring copy.
inline std::string ToUtf8(std::wstring_view src) {
    if (src.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, src.data(), (int)src.size(), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out((size_t)need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, src.data(), (int)src.size(), out.data(), need, nullptr, nullptr);
    return out;
}

inline std::wstring ToWide(const std::string& src) {
    if (src.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, src.c_str(), (int)src.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, src.c_str(), (int)src.size(), out.data(), need);
    return out;
}

// Case-insensitive "does haystack contain needle" over ASCII. Used for the
// device/monitor match strings the user types into the settings menu, and for
// matching an SMTC AUMID against a configured app name. Takes string_view so a
// caller can pass a slice (e.g. an app name with its ".exe" trimmed) without
// allocating a temporary.
inline bool ContainsNoCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    auto lower = [](char c) { return (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); };
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && lower(haystack[i + k]) == lower(needle[k])) ++k;
        if (k == needle.size()) return true;
    }
    return false;
}
