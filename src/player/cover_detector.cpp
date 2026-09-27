#include "cover_detector.h"

#include <dwmapi.h>

static HWND g_controller = nullptr;

bool CoverDetector::Start(HWND controller) {
    g_controller = controller;
    const std::pair<DWORD, DWORD> ranges[] = {
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},  // destroy, show, hide
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
    };
    for (auto [lo, hi] : ranges) {
        HWINEVENTHOOK h = SetWinEventHook(lo, hi, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (h) hooks_.push_back(h);
    }
    return !hooks_.empty();
}

void CoverDetector::Stop() {
    for (auto h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
}

void CALLBACK CoverDetector::OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND) {
        // Most object events are for child controls, carets, and cursors; only top-level windows matter.
        if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !hwnd) return;
        if (GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()) return;
    }
    PostMessageW(g_controller, WM_APP_COVER_DIRTY, event == EVENT_SYSTEM_FOREGROUND, 0);  // wParam: focus changed
}

static bool ShouldIgnore(HWND hwnd, const wchar_t* ignoredClass) {
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return true;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return true;

    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return true;  // click-through overlays (GPU/Steam/Discord overlays)
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags)) {
            if ((flags & LWA_COLORKEY) || ((flags & LWA_ALPHA) && alpha < 255)) return true;
        }
    }

    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    static const wchar_t* const kShell[] = {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};
    for (auto s : kShell)
        if (wcscmp(cls, s) == 0) return true;
    if (ignoredClass && wcscmp(cls, ignoredClass) == 0) return true;
    return false;
}

std::vector<bool> CoverDetector::ComputeCovered(const std::vector<RECT>& areas, const wchar_t* ignoredClass) {
    struct State {
        std::vector<HRGN> remaining;
        std::vector<bool> covered;
        size_t coveredCount = 0;
        const wchar_t* ignoredClass;
    } st;
    st.ignoredClass = ignoredClass;
    st.covered.assign(areas.size(), false);

    // Shrink each area a little so window borders and snap-layout gaps don't count as "visible".
    constexpr int kSlack = 8;
    for (const RECT& r : areas)
        st.remaining.push_back(CreateRectRgn(r.left + kSlack, r.top + kSlack, r.right - kSlack, r.bottom - kSlack));

    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& s = *reinterpret_cast<State*>(param);
            if (ShouldIgnore(hwnd, s.ignoredClass)) return TRUE;
            RECT wr;
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr))))
                GetWindowRect(hwnd, &wr);
            if (wr.right <= wr.left || wr.bottom <= wr.top) return TRUE;

            HRGN win = CreateRectRgnIndirect(&wr);
            for (size_t i = 0; i < s.remaining.size(); ++i) {
                if (s.covered[i]) continue;
                if (CombineRgn(s.remaining[i], s.remaining[i], win, RGN_DIFF) == NULLREGION) {
                    s.covered[i] = true;
                    ++s.coveredCount;
                }
            }
            DeleteObject(win);
            return s.coveredCount < s.covered.size();  // stop once everything is covered
        },
        reinterpret_cast<LPARAM>(&st));

    for (HRGN r : st.remaining) DeleteObject(r);
    return st.covered;
}
