// Wallpaper Plus settings window: a native window hosting the web UI (Microsoft Edge WebView2).
// Runs only while open; closing it exits the process completely. The always-running player
// (WallpaperPlus.exe) picks up changes by watching wallpaper.ini.

#include "bridge.h"
#include "paths.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>
#include <algorithm>
#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static const wchar_t kClass[] = L"WallpaperPlus.Settings";  // the player looks for this to focus us
static const COLORREF kBackground = RGB(9, 10, 14);           // matches the UI's page background

static HWND g_hwnd = nullptr;
static ComPtr<ICoreWebView2Controller> g_controller;
static ComPtr<ICoreWebView2> g_webview;
static Bridge g_bridge;

static void ResizeWebView() {
    if (!g_controller) return;
    RECT r;
    GetClientRect(g_hwnd, &r);
    g_controller->put_Bounds(r);
}

// While minimized the page is hidden and asked to use little memory; no hover videos or
// animations run, so it costs nothing.
static void SetWebViewActive(bool active) {
    if (!g_controller) return;
    g_controller->put_IsVisible(active);
    ComPtr<ICoreWebView2_19> wv19;
    if (g_webview && SUCCEEDED(g_webview.As(&wv19)))
        wv19->put_MemoryUsageTargetLevel(active ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL
                                                : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) {
                // Keep Windows' frame on the left, right and bottom (its invisible resize borders
                // and snapping), but give the caption strip to the page, which draws its own
                // title bar. A maximized window hangs over the screen edge by the frame width, so
                // its top is pulled back in by that much.
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const LONG top = p->rgrc[0].top;
                LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
                p->rgrc[0].top = top;
                if (IsZoomed(hwnd)) {
                    UINT dpi = GetDpiForWindow(hwnd);
                    p->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                                      GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                }
                return r;
            }
            break;
        case WM_APP_WINDOW:
            if (wp == kMinimize) ShowWindow(hwnd, SW_MINIMIZE);
            if (wp == kToggleMaximize) ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            if (wp == kClose) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_APP_BRIDGE:
            g_bridge.Deliver(lp);
            return 0;
        case WM_ACTIVATE:
            g_bridge.SendWindowState();
            break;
        case WM_SIZE:
            if (wp == SIZE_MINIMIZED) {
                SetWebViewActive(false);
            } else {
                SetWebViewActive(true);
                ResizeWebView();
                g_bridge.SendWindowState();
            }
            return 0;
        case WM_MOVE:
            if (g_controller) g_controller->NotifyParentWindowPositionChanged();
            return 0;
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            UINT dpi = GetDpiForWindow(hwnd);
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {MulDiv(980, dpi, 96), MulDiv(660, dpi, 96)};
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Dark window menu and a quiet 1 px border; the caption itself is drawn by the page.
static void ApplyDarkFrame(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF border = RGB(30, 33, 44);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

static void CreateWebView(const std::wstring& exeDir) {
    wchar_t localAppData[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::wstring userData = std::wstring(localAppData) + L"\\WallpaperPlus\\WebView2";

    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [exeDir](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result)) {
                    MessageBoxW(g_hwnd, L"Microsoft Edge WebView2 couldn't start.", L"Wallpaper Plus", MB_ICONERROR);
                    return result;
                }
                return env->CreateCoreWebView2Controller(
                    g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [exeDir](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) return result;
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            ComPtr<ICoreWebView2Controller2> c2;
                            if (SUCCEEDED(g_controller.As(&c2)))
                                c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 9, 10, 14});  // = kBackground

                            wchar_t devtools[8] = {};
                            const bool dev = GetEnvironmentVariableW(L"WALLPAPERPLUS_DEVTOOLS", devtools, 8) > 0;
                            ComPtr<ICoreWebView2Settings> settings;
                            g_webview->get_Settings(&settings);
                            settings->put_AreDefaultContextMenusEnabled(dev);
                            settings->put_AreDevToolsEnabled(dev);
                            settings->put_IsStatusBarEnabled(FALSE);
                            settings->put_IsZoomControlEnabled(FALSE);
                            ComPtr<ICoreWebView2Settings3> s3;
                            if (SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(dev);
                            // Lets the page mark its title bar "app-region: drag": dragging it moves
                            // the window (with snapping), double-click maximizes, right-click opens
                            // the window menu.
                            ComPtr<ICoreWebView2Settings9> s9;
                            if (SUCCEEDED(settings.As(&s9))) s9->put_IsNonClientRegionSupportEnabled(TRUE);

                            ComPtr<ICoreWebView2_3> wv3;
                            if (SUCCEEDED(g_webview.As(&wv3)))
                                wv3->SetVirtualHostNameToFolderMapping(L"app.wallpaperplus", (exeDir + L"\\ui").c_str(),
                                                                       COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            g_bridge.Attach(g_hwnd, g_webview.Get(), exeDir);
                            ResizeWebView();
                            g_webview->Navigate(L"https://app.wallpaperplus/index.html");
                            return S_OK;
                        })
                        .Get());
            })
            .Get());
    if (FAILED(hr)) {
        if (MessageBoxW(nullptr,
                        L"Wallpaper Plus needs the Microsoft Edge WebView2 Runtime, which isn't installed.\n\n"
                        L"Open the download page?",
                        L"Wallpaper Plus", MB_YESNO | MB_ICONERROR) == IDYES)
            ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", nullptr, nullptr,
                          SW_SHOWNORMAL);
        PostQuitMessage(1);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int show) {
    if (RefuseToRunFromArchive()) return 0;
    if (HWND existing = FindWindowW(kClass, nullptr)) {  // already open: bring it forward
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(kBackground);  // no white flash before the page paints
    RegisterClassExW(&wc);

    // Open centered on the monitor under the mouse, sized for its DPI.
    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    g_hwnd = CreateWindowExW(0, kClass, L"Wallpaper Plus", WS_OVERLAPPEDWINDOW, mi.rcWork.left, mi.rcWork.top, 100, 100,
                             nullptr, nullptr, inst, nullptr);
    const UINT dpi = GetDpiForWindow(g_hwnd);
    const RECT& wa = mi.rcWork;
    int w = std::min<int>(MulDiv(1320, dpi, 96), (wa.right - wa.left) * 92 / 100);
    int h = std::min<int>(MulDiv(860, dpi, 96), (wa.bottom - wa.top) * 92 / 100);
    SetWindowPos(g_hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);  // FRAMECHANGED: drop the caption now
    ApplyDarkFrame(g_hwnd);
    // Always open visible: launchers sometimes pass a minimized show state, which would make the
    // window appear to "not open".
    (void)show;
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);

    CreateWebView(ExeDirectory());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_controller.Reset();
    g_webview.Reset();
    CoUninitialize();
    return (int)msg.wParam;
}
