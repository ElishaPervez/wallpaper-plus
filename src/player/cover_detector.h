#pragma once
#include <windows.h>
#include <vector>

// Asks Windows for window move/resize/show/hide/minimize/cloak notifications and, on change,
// posts WM_APP_COVER_DIRTY (wParam = 1 when focus changed) to the controller window. The controller throttles and then calls
// ComputeCovered. No polling happens here.
class CoverDetector {
public:
    static constexpr UINT WM_APP_COVER_DIRTY = WM_APP + 10;

    bool Start(HWND controller);
    void Stop();

    // For each rect (a monitor's work area in screen pixels), true if visible top-level windows
    // together cover it completely. Windows whose class is ignoredClass are skipped.
    static std::vector<bool> ComputeCovered(const std::vector<RECT>& areas, const wchar_t* ignoredClass);

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);
    std::vector<HWINEVENTHOOK> hooks_;
};
