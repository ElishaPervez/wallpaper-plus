#pragma once
#include <windows.h>

// The spot between the static wallpaper and the desktop icons where our video windows live.
//
// Classic layout (Windows 10 / Windows 11 before 24H2): after the spawn message, Explorer
// creates a separate top-level WorkerW behind the icons; we become its children.
//
// Raised-desktop layout (Windows 11 24H2+): Progman itself holds the icon view and a WorkerW
// child. We become children of Progman, stacked below the icon view and above the WorkerW.
struct DesktopLayer {
    HWND parent = nullptr;   // window our surfaces are parented to
    HWND iconView = nullptr; // SHELLDLL_DefView (raised layout only)
    HWND workerW = nullptr;
    bool raised = false;
};

bool FindDesktopLayer(DesktopLayer& out);

// Puts an already-created child of layer.parent at the right depth.
void PlaceInLayer(HWND child, const DesktopLayer& layer);

// Makes Explorer repaint the static wallpaper after our windows are gone.
void RestoreStaticWallpaper(const DesktopLayer& layer);
