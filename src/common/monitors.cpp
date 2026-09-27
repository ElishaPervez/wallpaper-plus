#include "monitors.h"

#include <algorithm>
#include <map>

// Maps GDI device names (\\.\DISPLAY1) to the monitor's EDID model name.
static std::map<std::wstring, std::wstring> FriendlyNames() {
    std::map<std::wstring, std::wstring> out;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return out;
    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[i].targetInfo.adapterId;
        target.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
            DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0])
            out[source.viewGdiDeviceName] = target.monitorFriendlyDeviceName;
    }
    return out;
}

std::vector<MonitorInfo> EnumerateMonitors(bool withNames) {
    std::vector<MonitorInfo> out;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR mon, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) {
                MonitorInfo m;
                m.rect = mi.rcMonitor;
                m.work = mi.rcWork;
                m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                m.device = mi.szDevice;
                reinterpret_cast<std::vector<MonitorInfo>*>(param)->push_back(m);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    std::sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
    });

    if (withNames) {
        auto names = FriendlyNames();
        for (size_t i = 0; i < out.size(); ++i) {
            auto it = names.find(out[i].device);
            out[i].name = it != names.end() ? it->second : L"Display " + std::to_wstring(i + 1);
        }
    }
    return out;
}
