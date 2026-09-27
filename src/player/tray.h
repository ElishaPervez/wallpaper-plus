#pragma once
#include <windows.h>

// Notification-area icon. Left click opens the settings window; right click shows a small menu.
// Events arrive at the owner window as WM_APP_TRAY and are decoded by HandleMessage.
class Tray {
public:
    static constexpr UINT WM_APP_TRAY = WM_APP + 30;
    enum class Action { None, OpenSettings, TogglePause, Quit };

    void Add(HWND owner, HICON icon);  // also call again after Explorer restarts
    void Remove();
    void SetPaused(bool paused);       // updates the tooltip
    Action HandleMessage(WPARAM wp, LPARAM lp, bool paused);

private:
    Action ShowMenu(POINT at, bool paused);

    HWND owner_ = nullptr;
    HICON icon_ = nullptr;
    bool added_ = false;
};
