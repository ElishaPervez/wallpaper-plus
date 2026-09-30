// Wallpaper Plus player: a minimal video wallpaper for Windows 10/11.
//
// The controller runs on the main thread with a hidden top-level window (hidden top-level rather
// than message-only, because message-only windows don't receive the TaskbarCreated and
// WM_DISPLAYCHANGE broadcasts). It owns the tray icon and one surface window + Player per monitor.
// The settings window is a separate program (WallpaperPlusSettings.exe) launched from the tray;
// it talks to the player only by rewriting wallpaper.ini, which the player watches. The player
// reports back which decoder each monitor uses in player-status.txt.

#include "config.h"
#include "cover_detector.h"
#include "desktop_layer.h"
#include "gpu.h"
#include "log.h"
#include "monitors.h"
#include "paths.h"
#include "player.h"
#include "tray.h"
#include "uninstall.h"

#include <windows.h>
#include <mfapi.h>
#include <tlhelp32.h>
#include <wtsapi32.h>
#include <algorithm>
#include <memory>
#include <thread>
#include <vector>

static const wchar_t kControllerClass[] = L"WallpaperPlus.Controller";
static const wchar_t kSurfaceClass[] = L"WallpaperPlus.Surface";
static const wchar_t kSettingsClass[] = L"WallpaperPlus.Settings";  // settings window's class
static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t kRunValue[] = L"WallpaperPlus";

// Sent by a second launch of the exe: "open the settings window".
static constexpr UINT WM_APP_OPEN_SETTINGS = WM_APP + 40;

enum TimerId : UINT_PTR { kTimerCover = 1, kTimerConfig, kTimerRebuild, kTimerHealth };

// How long quitting or a rebuild waits for the monitors' players to stop, all together.
static constexpr DWORD kStopWaitMs = 3000;
// Quitting ends the process after this long whatever is still running, well inside the 10 s the
// installer waits for it to go.
static constexpr DWORD kExitDeadlineMs = 8000;

static void ApplyAutostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

// Brings up the settings window: focuses it if it's already open, otherwise launches it.
static void OpenSettingsWindow() {
    if (HWND existing = FindWindowW(kSettingsClass, nullptr)) {
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return;
    }
    std::wstring exe = ExeDirectory() + L"\\WallpaperPlusSettings.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    AllowSetForegroundWindow(ASFW_ANY);  // let the new window take focus
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Log(L"Can't start %s (error %lu)", exe.c_str(), GetLastError());
    }
}

// The tray draws at the small-icon size (16 px at 100% scaling). LoadIcon would hand it the
// 32 px image to shrink, which blurs it; this picks the frame drawn for that exact size.
static HICON LoadTrayIcon(HINSTANCE inst) {
    return static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                         LR_DEFAULTCOLOR | LR_SHARED));
}

static bool AnyProcessRunning(const std::vector<std::wstring>& exeNames) {
    if (exeNames.empty()) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        std::wstring name = pe.szExeFile;
        for (auto& c : name) c = (wchar_t)towlower(c);
        found = std::find(exeNames.begin(), exeNames.end(), name) != exeNames.end();
    }
    CloseHandle(snap);
    return found;
}

// True if the focused window fills its whole monitor (games, fullscreen video).
static bool ForegroundIsFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == GetDesktopWindow() || fg == GetShellWindow()) return false;
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT wr;
    MONITORINFO mi{sizeof(mi)};
    if (!GetWindowRect(fg, &wr) || !GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

static LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;  // never take mouse input from the desktop
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: ValidateRect(hwnd, nullptr); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

class App {
public:
    int Run(HINSTANCE inst);

private:
    static LRESULT CALLBACK ControllerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Build();
    void Teardown();
    void StartMonitor(size_t i);
    bool AnyPlayerStuck();
    PlaybackSettings SettingsFor(size_t i) const { return {config_.ForMonitor((int)i + 1), config_.fpsCap}; }
    void ReloadConfig(bool force);
    void RecomputeCover();
    void UpdatePause();
    void TogglePause();
    void ScheduleRebuild(UINT ms) { SetTimer(controller_, kTimerRebuild, ms, nullptr); }
    void WriteStatus();

    HINSTANCE inst_ = nullptr;
    HWND controller_ = nullptr;
    std::wstring configPath_;
    std::wstring statusPath_;
    FILETIME configStamp_{};
    Config config_;

    std::unique_ptr<GpuSet> gpus_ = std::make_unique<GpuSet>();
    DesktopLayer layer_;
    std::vector<MonitorInfo> monitors_;
    std::vector<HWND> surfaces_;
    std::vector<std::unique_ptr<Player>> players_;
    std::vector<Player*> abandoned_;  // stopped too slowly; never freed, see Teardown
    std::vector<bool> covered_;

    Tray tray_;
    CoverDetector cover_;
    bool coverPending_ = false;
    bool appCheckPending_ = false;
    bool pausedApp_ = false;         // a "pause for" app is running
    bool fullscreenActive_ = false;  // the focused window is fullscreen
    bool sessionLocked_ = false;
    bool displayOff_ = false;
    UINT taskbarCreatedMsg_ = 0;
};

void App::StartMonitor(size_t i) {
    const RECT& r = monitors_[i].rect;
    const int w = r.right - r.left, h = r.bottom - r.top;
    POINT origin = {r.left, r.top};
    MapWindowPoints(HWND_DESKTOP, layer_.parent, &origin, 1);  // parent spans the whole virtual screen

    HWND s = CreateWindowExW(WS_EX_NOACTIVATE, kSurfaceClass, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS,
                             origin.x, origin.y, w, h, layer_.parent, nullptr, inst_, nullptr);
    if (!s) {
        Log(L"Monitor %zu: CreateWindowEx failed %lu", i + 1, GetLastError());
        return;
    }
    PlaceInLayer(s, layer_);
    surfaces_[i] = s;
    HMONITOR monitor = MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST);
    players_[i] = std::make_unique<Player>(*gpus_, monitor, s, w, h, SettingsFor(i), controller_, (int)i + 1);
}

void App::Build() {
    Teardown();
    KillTimer(controller_, kTimerRebuild);

    if (!gpus_->Init(config_.decoder)) {
        Log(L"No graphics device could be created; retrying");
        ScheduleRebuild(5000);
        return;
    }
    if (!FindDesktopLayer(layer_)) {
        ScheduleRebuild(2000);
        return;
    }
    monitors_ = EnumerateMonitors();
    Log(L"Found %zu monitor(s)", monitors_.size());
    surfaces_.assign(monitors_.size(), nullptr);
    players_.resize(monitors_.size());
    covered_.assign(monitors_.size(), false);
    for (size_t i = 0; i < monitors_.size(); ++i) StartMonitor(i);
    RecomputeCover();
}

// All monitors are asked to stop first, then waited for together, so their wind-downs overlap.
// Players release their swap chains on their own threads before their windows go.
//
// A player that doesn't finish in time is stuck in a call into Windows' media code that stop can't
// cut short. It's left behind rather than waited for: the Player object and the GpuSet it draws
// with are never freed, so whenever the call does return, the thread finds everything it uses
// still there, sees the stop request and exits. Its window is destroyed now, which only makes its
// last drawing calls fail. The rebuild carries on with fresh devices.
void App::Teardown() {
    for (auto& player : players_)
        if (player) player->RequestStop();
    const ULONGLONG deadline = GetTickCount64() + kStopWaitMs;
    bool leftBehind = false;
    for (size_t i = 0; i < players_.size(); ++i) {
        if (players_[i]) {
            const ULONGLONG now = GetTickCount64();
            if (!players_[i]->WaitStopped(now < deadline ? (DWORD)(deadline - now) : 0)) {
                Log(L"Monitor %zu: player didn't stop within %lu ms (a file on a drive that stopped responding?); "
                    L"leaving it behind", i + 1, kStopWaitMs);
                abandoned_.push_back(players_[i].release());
                leftBehind = true;
            }
            players_[i].reset();
        }
        if (surfaces_[i]) DestroyWindow(surfaces_[i]);
    }
    players_.clear();
    surfaces_.clear();
    covered_.clear();
    if (leftBehind) {
        (void)gpus_.release();  // the stuck player may still use these devices
        gpus_ = std::make_unique<GpuSet>();
    } else {
        gpus_->Reset();
    }
    WriteStatus();
}

bool App::AnyPlayerStuck() {
    return std::any_of(abandoned_.begin(), abandoned_.end(), [](Player* p) { return !p->WaitStopped(0); });
}

// One line per playing monitor: number, then Player::Status() (device, hardware|processor, codec, size).
void App::WriteStatus() {
    std::wstring text;
    for (size_t i = 0; i < players_.size(); ++i) {
        if (!players_[i]) continue;
        std::wstring status = players_[i]->Status();
        if (!status.empty()) text += std::to_wstring(i + 1) + L"\t" + status + L"\r\n";
    }
    if (text.empty()) {
        DeleteFileW(statusPath_.c_str());
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), utf8.data(), n, nullptr, nullptr);
    const std::wstring tmp = statusPath_ + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    const bool ok = WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr) && written == utf8.size();
    CloseHandle(f);
    if (ok) MoveFileExW(tmp.c_str(), statusPath_.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void App::ReloadConfig(bool force) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    const bool existed = GetFileAttributesExW(configPath_.c_str(), GetFileExInfoStandard, &attr) != 0;
    if (!force && CompareFileTime(&attr.ftLastWriteTime, &configStamp_) == 0) return;  // something else in the folder changed

    // Remember the timestamp from *before* reading: if another save lands while we read, its newer
    // timestamp won't match and its change notification triggers another reload.
    Config old = config_;
    config_ = LoadConfig(configPath_);
    if (!existed) GetFileAttributesExW(configPath_.c_str(), GetFileExInfoStandard, &attr);  // template just written
    configStamp_ = attr.ftLastWriteTime;
    if (force || old.autostart != config_.autostart) ApplyAutostart(config_.autostart);
    tray_.SetPaused(config_.paused);
    if (force) return;

    Log(L"Settings reloaded");
    if (old.decoder != config_.decoder) {  // players hold on to their GPUs: start over with the new ranking
        Build();
        pausedApp_ = AnyProcessRunning(config_.pauseFor);
        UpdatePause();
        return;
    }
    // Players apply changes in place (no window rebuild, so no flash of the static wallpaper).
    for (size_t i = 0; i < players_.size(); ++i)
        if (players_[i]) players_[i]->Update(SettingsFor(i));
    pausedApp_ = AnyProcessRunning(config_.pauseFor);
    UpdatePause();
}

void App::RecomputeCover() {
    std::vector<RECT> areas;
    for (auto& m : monitors_) areas.push_back(m.work);
    covered_ = CoverDetector::ComputeCovered(areas, kSurfaceClass);
    fullscreenActive_ = config_.pauseAllWhenFullscreen && ForegroundIsFullscreen();
    UpdatePause();
}

void App::UpdatePause() {
    const bool all = sessionLocked_ || displayOff_ || config_.paused || pausedApp_ || fullscreenActive_;
    for (size_t i = 0; i < players_.size(); ++i) {
        if (!players_[i]) continue;
        bool hidden = config_.pauseWhenCovered && i < covered_.size() && covered_[i];
        players_[i]->SetPaused(all || hidden);
    }
}

void App::TogglePause() {
    config_.paused = !config_.paused;
    Log(L"Pause all: %s", config_.paused ? L"on" : L"off");
    if (SaveConfig(configPath_, config_)) {  // so the settings window shows the same state
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        GetFileAttributesExW(configPath_.c_str(), GetFileExInfoStandard, &attr);
        configStamp_ = attr.ftLastWriteTime;  // our own write; don't reload it
    }
    tray_.SetPaused(config_.paused);
    UpdatePause();
}

LRESULT CALLBACK App::ControllerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams);
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return app && app->controller_ ? app->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_) {
        Log(L"Explorer restarted; reattaching");
        tray_.Add(controller_, LoadTrayIcon(inst_));
        ScheduleRebuild(1500);  // give Explorer a moment to rebuild the desktop
        return 0;
    }
    switch (msg) {
        case CoverDetector::WM_APP_COVER_DIRTY:
            if (wp) appCheckPending_ = true;  // focus changed: a game may have just started
            if (!coverPending_) {             // throttle: at most one recompute per 150 ms, e.g. while dragging
                coverPending_ = true;
                SetTimer(controller_, kTimerCover, 150, nullptr);
            }
            return 0;

        case Tray::WM_APP_TRAY:
            switch (tray_.HandleMessage(wp, lp, config_.paused)) {
                case Tray::Action::OpenSettings: OpenSettingsWindow(); break;
                case Tray::Action::TogglePause: TogglePause(); break;
                case Tray::Action::Quit: DestroyWindow(controller_); break;
                case Tray::Action::None: break;
            }
            return 0;

        case WM_APP_OPEN_SETTINGS:
            OpenSettingsWindow();
            return 0;

        case Player::WM_APP_DEVICE_LOST:
            ScheduleRebuild(1000);
            return 0;

        case Player::WM_APP_STATUS:
            WriteStatus();
            return 0;

        case WM_TIMER:
            if (wp != kTimerHealth) KillTimer(controller_, wp);  // the rest are one-shot
            if (wp == kTimerCover) {
                coverPending_ = false;
                if (appCheckPending_) {
                    appCheckPending_ = false;
                    pausedApp_ = AnyProcessRunning(config_.pauseFor);
                }
                RecomputeCover();
            } else if (wp == kTimerConfig) {
                ReloadConfig(false);
            } else if (wp == kTimerRebuild) {
                Build();
            } else if (wp == kTimerHealth) {
                // Backup checks in case a notification was missed; also catches apps started in the background.
                if (!layer_.parent || !IsWindow(layer_.parent)) {
                    Log(L"Desktop layer vanished; reattaching");
                    Build();
                } else {
                    pausedApp_ = AnyProcessRunning(config_.pauseFor);
                    RecomputeCover();
                }
            }
            return 0;

        case WM_DISPLAYCHANGE:
            Log(L"Display configuration changed");
            ScheduleRebuild(500);
            return 0;

        case WM_SETTINGCHANGE:
            if (wp == SPI_SETWORKAREA) {  // taskbar moved/resized: only the covered test changes
                auto fresh = EnumerateMonitors();
                if (fresh.size() == monitors_.size()) {
                    for (size_t i = 0; i < fresh.size(); ++i) monitors_[i].work = fresh[i].work;
                    RecomputeCover();
                } else {
                    ScheduleRebuild(500);
                }
            }
            return 0;

        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK || wp == WTS_CONSOLE_DISCONNECT) sessionLocked_ = true;
            if (wp == WTS_SESSION_UNLOCK || wp == WTS_CONSOLE_CONNECT) sessionLocked_ = false;
            UpdatePause();
            return 0;

        case WM_POWERBROADCAST:
            if (wp == PBT_POWERSETTINGCHANGE) {
                auto* s = reinterpret_cast<POWERBROADCAST_SETTING*>(lp);
                if (s->PowerSetting == GUID_CONSOLE_DISPLAY_STATE) {
                    displayOff_ = s->Data[0] == 0;  // 0 off, 1 on, 2 dimmed
                    UpdatePause();
                }
            }
            return TRUE;

        case WM_CLOSE:
            DestroyWindow(controller_);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(controller_, msg, wp, lp);
}

int App::Run(HINSTANCE inst) {
    inst_ = inst;
    const std::wstring dir = DataDirectory();
    configPath_ = dir + L"\\wallpaper.ini";
    statusPath_ = dir + L"\\player-status.txt";
    LogInit(dir + L"\\wallpaper-plus.log");
    Log(L"Wallpaper Plus starting");
    Log(L"Settings folder: %s", dir.c_str());

    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ControllerProc;
    wc.hInstance = inst;
    wc.lpszClassName = kControllerClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = SurfaceProc;
    wc.lpszClassName = kSurfaceClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kControllerClass, L"Wallpaper Plus", WS_POPUP, 0, 0, 0, 0, nullptr,
                                nullptr, inst, this);
    controller_ = hwnd;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd, WM_APP_OPEN_SETTINGS, MSGFLT_ALLOW, nullptr);
    WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);
    HPOWERNOTIFY power = RegisterPowerSettingNotification(hwnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);

    ReloadConfig(true);
    tray_.Add(hwnd, LoadTrayIcon(inst));
    tray_.SetPaused(config_.paused);
    pausedApp_ = AnyProcessRunning(config_.pauseFor);
    cover_.Start(hwnd);
    Build();
    SetTimer(hwnd, kTimerHealth, 5000, nullptr);

    bool anyVideo = !config_.defaults.videos.empty();
    for (const auto& [n, m] : config_.perMonitor) anyVideo = anyVideo || !m.videos.empty();
    if (!anyVideo) OpenSettingsWindow();  // first run: go straight to picking a wallpaper

    // Watch the settings folder for saves to wallpaper.ini.
    HANDLE watch = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

    MSG msg;
    bool running = true;
    while (running) {
        DWORD r = MsgWaitForMultipleObjects(watch != INVALID_HANDLE_VALUE ? 1 : 0, &watch, FALSE, INFINITE, QS_ALLINPUT);
        if (watch != INVALID_HANDLE_VALUE && r == WAIT_OBJECT_0) {
            FindNextChangeNotification(watch);
            SetTimer(hwnd, kTimerConfig, 200, nullptr);  // editors save in several steps; wait for the last
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    Log(L"Shutting down");
    // Last resort, if something below hangs anyway: the tray icon is already gone at that point, so
    // a process that never ends would be invisible, and would block installing an update.
    std::thread([] {
        Sleep(kExitDeadlineMs);
        Log(L"Still shutting down after %lu ms; ending the process", kExitDeadlineMs);
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
    if (watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(watch);
    tray_.Remove();
    cover_.Stop();
    DesktopLayer layer = layer_;
    Teardown();
    if (layer.parent && IsWindow(layer.parent)) RestoreStaticWallpaper(layer);
    if (power) UnregisterPowerSettingNotification(power);
    if (AnyPlayerStuck()) {
        // Its thread is still inside Media Foundation: shutting that down under it could crash it,
        // and the normal exit would wait for it. The wallpaper is already back; just end.
        Log(L"A player is still stuck; ending the process");
        TerminateProcess(GetCurrentProcess(), 0);
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    // "WallpaperPlus.exe --quit" stops the running copy.
    if (wcsstr(cmdLine, L"--quit")) {
        if (HWND running = FindWindowW(kControllerClass, nullptr)) PostMessageW(running, WM_CLOSE, 0, 0);
        return 0;
    }
    // Run by the uninstaller; see uninstall.h.
    if (wcsstr(cmdLine, L"--uninstall-cleanup")) return UninstallCleanup(wcsstr(cmdLine, L"--delete-data") != nullptr);

    if (RefuseToRunFromArchive()) return 0;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WallpaperPlus.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Launched again while running (e.g. double-clicked): open the settings window instead.
        if (HWND running = FindWindowW(kControllerClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(running, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(running, WM_APP_OPEN_SETTINGS, 0, 0);
        }
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int code = App().Run(inst);
    CloseHandle(mutex);
    return code;
}
