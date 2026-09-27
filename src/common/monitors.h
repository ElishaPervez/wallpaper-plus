#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct MonitorInfo {
    RECT rect;                 // full monitor, physical screen pixels
    RECT work;                 // excluding the taskbar
    bool primary = false;
    std::wstring device;       // e.g. \\.\DISPLAY1
    std::wstring name;         // the model name the monitor reports (falls back to "Display N")
};

// Monitors sorted left to right (top to bottom for stacked ones). Both the player and the settings
// window number monitors by position in this list, starting at 1.
// The calling process must be per-monitor DPI aware for rects to be in physical pixels.
std::vector<MonitorInfo> EnumerateMonitors(bool withNames = false);
