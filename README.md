SmartVolumeMixer (XPG Summoner Edition)

A lightweight, high-performance C++ utility designed to fix mechanical volume wheel "drumming" and provide an elegant, customizable Windows OSD with intelligent media session filtering.

About the Project

Born out of a need to tame the erratic behavior of mechanical volume rollers (specifically the XPG Summoner keyboard), this project transforms basic Windows volume control into a premium, responsive experience. It combines low-level keyboard hooks with modern Windows Media Session APIs to give you precise control over your audio and media without the "bouncing" or accidental triggers common in hardware.

Key Features

🎯 Intelligent Debouncing: A custom streaming-debounce algorithm that eliminates erratic volume jumps while maintaining high responsiveness for fast scrolls.

🖼️ Premium OSD: A sleek, non-intrusive On-Screen Display built with low-latency graphics for real-time volume and track feedback.

🔍 App-Specific Filtering: Configure which apps respond to your media keys (e.g., control Spotify while ignoring Telegram or browser videos).

⚡ Ultra-Lightweight: Written in native C++ for minimal CPU/RAM footprint and near-zero latency.

🛠️ Robust Reliability: Features automatic crash recovery and system tray integration for a "set it and forget it" background experience.

⌨️ Advanced Input Logic: Support for Shift-modified volume jumps and customizable tick thresholds.

Tech Stack

Language: C++17

APIs: Win32, WASAPI, SMTC (System Media Transport Controls)

Build System: CMake

UI Framework: ImGui

Donations:

PayPal:

vad.sofr@gmail.com
