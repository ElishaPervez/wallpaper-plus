#include "tray.h"

#include <shellapi.h>
#include <windowsx.h>

static constexpr UINT kIconId = 1;

void Tray::Add(HWND owner, HICON icon) {
    owner_ = owner;
    icon_ = icon;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"Wallpaper Plus");
    Shell_NotifyIconW(NIM_DELETE, &nid);  // in case a stale icon survived an Explorer restart
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void Tray::Remove() {
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = owner_;
    nid.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void Tray::SetPaused(bool paused) {
    if (!added_) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = owner_;
    nid.uID = kIconId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcscpy_s(nid.szTip, paused ? L"Wallpaper Plus (paused)" : L"Wallpaper Plus");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

Tray::Action Tray::HandleMessage(WPARAM wp, LPARAM lp, bool paused) {
    // With NOTIFYICON_VERSION_4: LOWORD(lp) is the event, wp carries the anchor point.
    switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            return Action::OpenSettings;
        case WM_CONTEXTMENU:
            return ShowMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)}, paused);
    }
    return Action::None;
}

Tray::Action Tray::ShowMenu(POINT at, bool paused) {
    enum : UINT { kOpen = 1, kPause, kQuit };
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpen, L"Open Wallpaper Plus");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (paused ? MF_CHECKED : 0), kPause, L"Pause wallpapers");
    AppendMenuW(menu, MF_STRING, kQuit, L"Quit");
    SetMenuDefaultItem(menu, kOpen, FALSE);

    SetForegroundWindow(owner_);  // required so the menu closes when clicking elsewhere
    UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y,
                                owner_, nullptr);
    DestroyMenu(menu);
    switch (cmd) {
        case kOpen: return Action::OpenSettings;
        case kPause: return Action::TogglePause;
        case kQuit: return Action::Quit;
    }
    return Action::None;
}
