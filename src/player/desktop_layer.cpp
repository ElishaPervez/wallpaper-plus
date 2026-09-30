#include "desktop_layer.h"
#include "log.h"

#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Undocumented Progman message that makes Explorer create the WorkerW behind the icons.
static constexpr UINT kSpawnWorkerW = 0x052C;

bool FindDesktopLayer(DesktopLayer& out) {
    out = {};
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        Log(L"Desktop: Progman not found (Explorer not running?)");
        return false;
    }

    DWORD_PTR result;
    SendMessageTimeoutW(progman, kSpawnWorkerW, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
    SendMessageTimeoutW(progman, kSpawnWorkerW, 0, 0, SMTO_NORMAL, 1000, &result);

    // Raised-desktop layout: the icon view is a direct child of Progman.
    HWND iconView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
    if (iconView) {
        HWND worker = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        if (!worker) {
            Log(L"Desktop: raised layout but no WorkerW child");
            return false;
        }
        out = {progman, iconView, worker, true};
        Log(L"Desktop: raised layout (Windows 11 24H2+)");
        return true;
    }

    // Classic layout: find the top-level window holding the icon view; the WorkerW right after it
    // in z-order is the empty layer behind the icons.
    HWND worker = nullptr;
    EnumWindows(
        [](HWND top, LPARAM param) -> BOOL {
            if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr)) {
                *reinterpret_cast<HWND*>(param) = FindWindowExW(nullptr, top, L"WorkerW", nullptr);
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&worker));
    if (!worker) {
        Log(L"Desktop: classic layout but no WorkerW found");
        return false;
    }
    out = {worker, nullptr, worker, false};
    Log(L"Desktop: classic layout");
    return true;
}

void PlaceInLayer(HWND child, const DesktopLayer& layer) {
    if (!layer.raised) return;  // children of the classic WorkerW need no z-order work
    constexpr UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
    SetWindowPos(child, layer.iconView, 0, 0, 0, 0, flags);  // just below the icons
    // Static wallpaper under every surface. (Inserting it right after `child` would bury
    // surfaces placed earlier for other monitors.)
    SetWindowPos(layer.workerW, HWND_BOTTOM, 0, 0, 0, 0, flags);
}

void RestoreStaticWallpaper(const DesktopLayer& layer) {
    if (layer.raised) {
        // The static wallpaper sits untouched in the WorkerW under us; a repaint is enough.
        RedrawWindow(layer.parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        return;
    }
    // Classic layout: the WorkerW doesn't repaint the wallpaper by itself; Explorer redraws it when a
    // wallpaper is applied. Each monitor gets back the picture it already has (applying one path to
    // the whole desktop would put the same picture on every monitor and end a slideshow), and a
    // slideshow is just moved on to its next picture, so it keeps running.
    ComPtr<IDesktopWallpaper> wallpaper;
    if (FAILED(CoCreateInstance(__uuidof(DesktopWallpaper), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wallpaper)))) {
        Log(L"Desktop: can't reach the wallpaper settings; the last video frame may stay until the wallpaper changes");
        return;
    }
    DESKTOP_SLIDESHOW_STATE state = {};
    if (SUCCEEDED(wallpaper->GetStatus(&state)) && (state & DSS_ENABLED) && (state & DSS_SLIDESHOW)) {
        wallpaper->AdvanceSlideshow(nullptr, DSD_FORWARD);
        return;
    }
    // One picture on every monitor (also a picture spanned across them) answers for "all monitors";
    // different pictures per monitor answer S_FALSE there and are applied one by one.
    LPWSTR shared = nullptr;
    bool applied = wallpaper->GetWallpaper(nullptr, &shared) == S_OK && shared && *shared &&
                   SUCCEEDED(wallpaper->SetWallpaper(nullptr, shared));
    CoTaskMemFree(shared);
    UINT count = 0;
    if (!applied) wallpaper->GetMonitorDevicePathCount(&count);
    for (UINT i = 0; i < count; ++i) {
        LPWSTR monitor = nullptr, path = nullptr;
        RECT r;
        if (SUCCEEDED(wallpaper->GetMonitorDevicePathAt(i, &monitor)) &&
            SUCCEEDED(wallpaper->GetMonitorRECT(monitor, &r)) &&  // fails for monitors that aren't connected
            SUCCEEDED(wallpaper->GetWallpaper(monitor, &path)) && path && *path)
            applied = SUCCEEDED(wallpaper->SetWallpaper(monitor, path)) || applied;
        CoTaskMemFree(path);
        CoTaskMemFree(monitor);
    }
    // No picture anywhere: a solid color. Re-applying "no picture" repaints it and loses nothing.
    if (!applied) {
        wchar_t path[MAX_PATH] = {};
        if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0) && !path[0])
            SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, 0);
    }
}
