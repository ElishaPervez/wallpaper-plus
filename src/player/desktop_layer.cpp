#include "desktop_layer.h"
#include "log.h"

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
    // Classic layout: the WorkerW doesn't repaint the wallpaper by itself. Re-applying the current
    // wallpaper path makes Explorer redraw it.
    wchar_t path[MAX_PATH] = {};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0))
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, path, 0);
}
