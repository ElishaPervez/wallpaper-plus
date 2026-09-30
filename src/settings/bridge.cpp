#include "bridge.h"

#include "config.h"
#include "monitors.h"
#include "paths.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincrypt.h>
#include <wrl.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <vector>

using nlohmann::json;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static const wchar_t kControllerClass[] = L"WallpaperPlus.Controller";  // the player's hidden window
static const wchar_t* const kVideoExts[] = {L".mp4", L".m4v", L".mkv", L".webm", L".mov", L".avi", L".wmv"};

// ---------- small helpers ----------

static std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static bool IsVideo(const fs::path& p) {
    std::wstring ext = Lower(p.extension().wstring());
    return std::any_of(std::begin(kVideoExts), std::end(kVideoExts), [&](const wchar_t* e) { return ext == e; });
}

static bool WriteFileAtomic(const std::wstring& path, const std::string& bytes) {
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(bytes.data(), (std::streamsize)bytes.size());
        if (!f) return false;
    }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// "Couldn't save settings to C:\\...\\wallpaper.ini: Access is denied." Call right after the failure.
static std::runtime_error SaveError(const char* what, const std::wstring& path) {
    const DWORD code = GetLastError();
    std::string reason;
    wchar_t* text = nullptr;
    if (code && FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, code, 0, (LPWSTR)&text, 0, nullptr)) {
        std::wstring w = text;
        LocalFree(text);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
        reason = ": " + Utf8(w);
    }
    return std::runtime_error("Couldn't save " + std::string(what) + " to " + Utf8(path) + reason);
}

static std::vector<std::wstring> ScanFolder(const std::wstring& folder) {
    std::vector<std::wstring> out;
    std::error_code ec;
    fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end && out.size() < 2000; it.increment(ec)) {
        if (it.depth() > 3) it.disable_recursion_pending();
        if (it->is_regular_file(ec) && IsVideo(it->path())) out.push_back(it->path().wstring());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------- browse cache ----------

// Cache files are named by a hash of the site path, keeping the extension so the page's video
// and image elements recognise them.
static std::wstring CacheName(const std::string& path) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : path) h = (h ^ c) * 1099511628211ull;
    wchar_t name[32];
    swprintf_s(name, L"%016llx", (unsigned long long)h);
    return name + Wide(path.substr(path.find_last_of('.')));
}

// Keeps the cache from growing forever: past 400 MB, the oldest files go until it's under 300 MB.
// Leftover partial files from an interrupted run are removed too.
static void PruneCache(const std::wstring& dir) {
    struct Entry { fs::file_time_type time; uintmax_t size; fs::path path; };
    std::vector<Entry> files;
    uintmax_t total = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() == L".part") { fs::remove(e.path(), ec); continue; }
        uintmax_t size = e.file_size(ec);
        files.push_back({e.last_write_time(ec), size, e.path()});
        total += size;
    }
    if (total <= (400ull << 20)) return;
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.time < b.time; });
    for (auto& f : files) {
        if (total <= (300ull << 20)) break;
        if (fs::remove(f.path, ec)) total -= f.size;
    }
}

static std::wstring VideosFolder() {
    PWSTR videos = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &videos))) out = videos;
    CoTaskMemFree(videos);
    return out;
}

// Only characters that are safe in a Windows file name; the server's name is a suggestion.
static std::wstring SafeFileName(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name)
        if (iswalnum(c) || c == L'.' || c == L'-' || c == L'_') out += c;
    while (!out.empty() && out[0] == L'.') out.erase(0, 1);
    return out.size() > 150 ? out.substr(out.size() - 150) : out;
}

// ---------- config <-> json ----------

static const char* const kFitNames[] = {"fill", "fit", "stretch"};

static json ToJson(const MonitorSetting& m) {
    json videos = json::array();
    for (auto& v : m.videos) videos.push_back(Utf8(v));
    return {{"videos", videos},       {"fit", kFitNames[(int)m.fit]}, {"brightness", m.brightness},
            {"speed", m.speed},       {"rotateMinutes", m.rotateMinutes}, {"shuffle", m.shuffle}};
}

static MonitorSetting MonitorFromJson(const json& j) {
    MonitorSetting m;
    for (auto& v : j.value("videos", json::array())) m.videos.push_back(Wide(v.get<std::string>()));
    std::string fit = j.value("fit", "fill");
    m.fit = fit == "fit" ? FitMode::Fit : fit == "stretch" ? FitMode::Stretch : FitMode::Fill;
    m.brightness = std::clamp(j.value("brightness", 100), 10, 100);
    m.speed = std::clamp(j.value("speed", 1.0), 0.25, 2.0);
    m.rotateMinutes = std::max(0, j.value("rotateMinutes", 30));
    m.shuffle = j.value("shuffle", false);
    return m;
}

static json ToJson(const Config& c) {
    json pauseFor = json::array();
    for (auto& e : c.pauseFor) pauseFor.push_back(Utf8(e));
    json monitors = json::object();
    for (auto& [n, m] : c.perMonitor) monitors[std::to_string(n)] = ToJson(m);
    return {{"pauseWhenCovered", c.pauseWhenCovered},
            {"pauseAllWhenFullscreen", c.pauseAllWhenFullscreen},
            {"pauseFor", pauseFor},
            {"fpsCap", c.fpsCap},
            {"decoder", DecoderName(c.decoder)},
            {"autostart", c.autostart},
            {"paused", c.paused},
            {"defaults", ToJson(c.defaults)},
            {"monitors", monitors}};
}

static Config ConfigFromJson(const json& j) {
    Config c;
    c.pauseWhenCovered = j.value("pauseWhenCovered", true);
    c.pauseAllWhenFullscreen = j.value("pauseAllWhenFullscreen", false);
    for (auto& e : j.value("pauseFor", json::array())) c.pauseFor.push_back(Lower(Wide(e.get<std::string>())));
    c.fpsCap = std::clamp(j.value("fpsCap", 0), 0, 240);
    c.decoder = DecoderFromName(Wide(j.value("decoder", "auto")));
    c.autostart = j.value("autostart", true);
    c.paused = j.value("paused", false);
    if (j.contains("defaults")) c.defaults = MonitorFromJson(j["defaults"]);
    const json monitors = j.value("monitors", json::object());  // keep alive: items() doesn't own it
    for (auto& [key, value] : monitors.items()) {
        int n = atoi(key.c_str());
        if (n > 0) c.perMonitor[n] = MonitorFromJson(value);
    }
    return c;
}

// ---------- dialogs ----------

static std::vector<std::wstring> OpenDialog(HWND owner, bool folders, bool multi, const wchar_t* filterName,
                                            const wchar_t* filterSpec) {
    std::vector<std::wstring> out;
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | (folders ? FOS_PICKFOLDERS : 0) | (multi ? FOS_ALLOWMULTISELECT : 0);
    dlg->SetOptions(opts);
    if (!folders) {
        COMDLG_FILTERSPEC spec = {filterName, filterSpec};
        dlg->SetFileTypes(1, &spec);
    }
    if (FAILED(dlg->Show(owner))) return out;  // cancelled
    ComPtr<IShellItemArray> items;
    if (FAILED(dlg->GetResults(&items))) return out;
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out.push_back(path);
            CoTaskMemFree(path);
        }
    }
    return out;
}

static json RunningApps() {
    struct Ctx {
        std::map<std::wstring, std::wstring> apps;  // exe -> window title
        DWORD self;
    } ctx{{}, GetCurrentProcessId()};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto& c = *reinterpret_cast<Ctx*>(param);
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                return TRUE;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            wchar_t title[256] = {};
            if (cloaked || !GetWindowTextW(hwnd, title, 256)) return TRUE;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c.self) return TRUE;
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!proc) return TRUE;
            wchar_t path[MAX_PATH];
            DWORD len = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
                std::wstring exe = Lower(fs::path(path).filename().wstring());
                static const wchar_t* const kSkip[] = {L"explorer.exe", L"applicationframehost.exe",
                                                       L"systemsettings.exe", L"textinputhost.exe",
                                                       L"wallpaperplussettings.exe", L"msedgewebview2.exe"};
                if (std::none_of(std::begin(kSkip), std::end(kSkip), [&](const wchar_t* s) { return exe == s; }) &&
                    !c.apps.count(exe))
                    c.apps[exe] = title;
            }
            CloseHandle(proc);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    json out = json::array();
    for (auto& [exe, title] : ctx.apps) out.push_back({{"exe", Utf8(exe)}, {"title", Utf8(title)}});
    return out;
}

// ---------- bridge ----------

void Bridge::Attach(HWND hwnd, ICoreWebView2* webview, const std::wstring& exeDir) {
    hwnd_ = hwnd;
    webview_ = webview;
    webview_.As(&webview3_);
    exeDir_ = exeDir;
    dataDir_ = DataDirectory();  // %APPDATA%\WallpaperPlus, shared with the player
    configPath_ = dataDir_ + L"\\wallpaper.ini";
    libraryPath_ = dataDir_ + L"\\library.json";
    thumbsDir_ = dataDir_ + L"\\thumbs";
    webCacheDir_ = dataDir_ + L"\\webcache";
    CreateDirectoryW(thumbsDir_.c_str(), nullptr);
    CreateDirectoryW(webCacheDir_.c_str(), nullptr);
    if (webview3_) {
        webview3_->SetVirtualHostNameToFolderMapping(L"thumbs.wallpaperplus", thumbsDir_.c_str(),
                                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        webview3_->SetVirtualHostNameToFolderMapping(L"webcache.wallpaperplus", webCacheDir_.c_str(),
                                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }
    fetches_.Push(9, "prune", [dir = webCacheDir_](bool cancelled) {
        if (cancelled) return;
        PruneCache(dir);
        // Downloads cut off by closing the window leave a partial file behind.
        std::error_code ec;
        for (auto& e : fs::directory_iterator(VideosFolder() + L"\\Wallpaper Plus", ec))
            if (e.path().extension() == L".part") fs::remove(e.path(), ec);
    });
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26 && webview3_; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t host[32], root[4] = {wchar_t(L'A' + i), L':', L'\\', 0};
        swprintf_s(host, L"drive-%c.wallpaperplus", wchar_t(L'a' + i));
        webview3_->SetVirtualHostNameToFolderMapping(host, root, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    }

    // The page can read local files (that's how it previews videos from any drive), so it must never
    // be able to send anything off the machine: every request and navigation outside the app's own
    // *.wallpaperplus hosts is refused.
    auto isLocal = [](LPCWSTR uri) {
        std::wstring u = Lower(uri ? uri : L"");
        if (u.rfind(L"data:", 0) == 0 || u.rfind(L"blob:", 0) == 0 || u == L"about:blank") return true;
        if (u.rfind(L"https://", 0) != 0) return false;
        std::wstring host = u.substr(8, u.find_first_of(L"/?#:", 8) - 8);
        static const std::wstring kSuffix = L".wallpaperplus";
        return host.size() > kSuffix.size() && host.compare(host.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
    };
    EventRegistrationToken token;
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [isLocal](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                LPWSTR uri = nullptr;
                args->get_Uri(&uri);
                if (!isLocal(uri)) args->put_Cancel(TRUE);
                CoTaskMemFree(uri);
                return S_OK;
            })
            .Get(),
        &token);
    ComPtr<ICoreWebView2_2> webview2;
    ComPtr<ICoreWebView2Environment> env;
    if (SUCCEEDED(webview_.As(&webview2))) webview2->get_Environment(&env);
    webview_->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    webview_->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [isLocal, env](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) {
                ComPtr<ICoreWebView2WebResourceRequest> request;
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Request(&request))) request->get_Uri(&uri);
                if (!isLocal(uri) && env) {
                    ComPtr<ICoreWebView2WebResourceResponse> blocked;
                    env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &blocked);
                    args->put_Response(blocked.Get());
                }
                CoTaskMemFree(uri);
                return S_OK;
            })
            .Get(),
        &token);

    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) { return OnMessage(args); })
            .Get(),
        &token);
}

void Bridge::Send(const json& message) {
    if (!webview_) return;
    // Text from the site isn't guaranteed to be valid UTF-8; bad bytes become U+FFFD.
    webview_->PostWebMessageAsJson(Wide(message.dump(-1, ' ', false, json::error_handler_t::replace)).c_str());
}

void Bridge::Deliver(LPARAM message) {
    std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(message));
    if (webview_) webview_->PostWebMessageAsJson(Wide(*text).c_str());
}

static json WindowState(HWND hwnd) {
    return {{"maximized", IsZoomed(hwnd) != FALSE}, {"active", GetForegroundWindow() == hwnd}};
}

void Bridge::SendWindowState() {
    Send({{"event", "window"}, {"data", WindowState(hwnd_)}});
}

std::string Bridge::MediaUrl(const std::wstring& path) {
    // Local drives are served as https://drive-c.wallpaperplus/... (mapped in Attach, before the
    // page loads; mappings added later aren't visible to an already-loaded page). The host streams
    // files with seeking support. UNC paths aren't covered and simply get no preview.
    if (path.size() < 3 || path[1] != L':' || !iswalpha(path[0])) return {};
    std::string rel = Utf8(path.substr(3)), encoded;
    for (unsigned char c : rel) {
        if (c == '\\') {
            encoded += '/';
        } else if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            encoded += buf;
        }
    }
    return std::string("https://drive-") + (char)towlower(path[0]) + ".wallpaperplus/" + encoded;
}

HRESULT Bridge::OnMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR raw = nullptr;
    if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
    json msg = json::parse(Utf8(raw), nullptr, false);
    CoTaskMemFree(raw);
    if (msg.is_discarded() || !msg.is_object()) return S_OK;

    std::string cmd = msg.value("cmd", "");
    if (cmd == "drop") {  // files dragged onto the window arrive as attached objects
        json paths = json::array();
        ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
        ComPtr<ICoreWebView2ObjectCollectionView> objects;
        UINT count = 0;
        if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) &&
            SUCCEEDED(args2->get_AdditionalObjects(&objects)) && SUCCEEDED(objects->get_Count(&count))) {
            for (UINT i = 0; i < count; ++i) {
                ComPtr<IUnknown> obj;
                ComPtr<ICoreWebView2File> file;
                LPWSTR path = nullptr;
                if (SUCCEEDED(objects->GetValueAtIndex(i, &obj)) && SUCCEEDED(obj.As(&file)) &&
                    SUCCEEDED(file->get_Path(&path))) {
                    std::error_code ec;
                    if (fs::is_directory(path, ec)) {
                        for (auto& v : ScanFolder(path)) paths.push_back(Utf8(v));
                    } else if (IsVideo(path)) {
                        paths.push_back(Utf8(path));
                    }
                    CoTaskMemFree(path);
                }
            }
        }
        Send({{"event", "dropped"}, {"data", paths}});
        return S_OK;
    }

    if (cmd.rfind("web.", 0) == 0) {
        try {
            HandleWeb(msg.value("id", json()), cmd, msg.value("args", json::object()));
        } catch (const std::exception& e) {
            Send({{"id", msg.value("id", json())}, {"error", e.what()}});
        }
        return S_OK;
    }

    json reply = {{"id", msg.value("id", json())}};
    try {
        reply["result"] = Handle(cmd, msg.value("args", json::object()));
    } catch (const std::exception& e) {
        reply["error"] = e.what();
    }
    Send(reply);
    return S_OK;
}

json Bridge::Handle(const std::string& cmd, const json& args) {
    if (cmd == "getState") {
        json monitors = json::array();
        auto list = EnumerateMonitors(true);
        for (size_t i = 0; i < list.size(); ++i) {
            const RECT& r = list[i].rect;
            monitors.push_back({{"number", (int)i + 1},
                                {"name", Utf8(list[i].name)},
                                {"x", r.left},
                                {"y", r.top},
                                {"width", r.right - r.left},
                                {"height", r.bottom - r.top},
                                {"primary", list[i].primary}});
        }
        json library = json::object();
        std::ifstream f(libraryPath_, std::ios::binary);
        if (f) library = json::parse(f, nullptr, false);
        if (library.is_discarded() || !library.is_object()) library = json::object();
        return {{"monitors", monitors},
                {"config", ToJson(LoadConfig(configPath_))},
                {"library", library},
                {"playerRunning", FindWindowW(kControllerClass, nullptr) != nullptr}};
    }
    if (cmd == "setConfig") {
        if (!SaveConfig(configPath_, ConfigFromJson(args.at("config")))) throw SaveError("settings", configPath_);
        return true;
    }
    if (cmd == "saveLibrary") {
        if (!WriteFileAtomic(libraryPath_, args.at("library").dump(1))) throw SaveError("library", libraryPath_);
        return true;
    }
    if (cmd == "pickVideos") {
        json out = json::array();
        for (auto& p : OpenDialog(hwnd_, false, true, L"Videos", L"*.mp4;*.m4v;*.mkv;*.webm;*.mov;*.avi;*.wmv"))
            out.push_back(Utf8(p));
        return out;
    }
    if (cmd == "pickFolder") {
        auto picked = OpenDialog(hwnd_, true, false, nullptr, nullptr);
        if (picked.empty()) return nullptr;
        json videos = json::array();
        for (auto& v : ScanFolder(picked[0])) videos.push_back(Utf8(v));
        return {{"folder", Utf8(picked[0])}, {"videos", videos}};
    }
    if (cmd == "scanFolders") {
        json out = json::array();
        for (auto& folder : args.at("folders"))
            for (auto& v : ScanFolder(Wide(folder.get<std::string>()))) out.push_back(Utf8(v));
        return out;
    }
    if (cmd == "mediaUrls") {
        json out = json::array();
        for (auto& p : args.at("paths")) out.push_back(MediaUrl(Wide(p.get<std::string>())));
        return out;
    }
    if (cmd == "statFiles") {
        json out = json::array();
        for (auto& p : args.at("paths")) {
            std::error_code ec;
            out.push_back(fs::is_regular_file(Wide(p.get<std::string>()), ec));
        }
        return out;
    }
    if (cmd == "saveThumb") {
        std::string id = args.at("id"), data = args.at("dataUrl");
        if (id.empty() || id.size() > 40 || !std::all_of(id.begin(), id.end(), [](char c) { return isalnum((unsigned char)c); }))
            throw std::runtime_error("bad thumbnail id");
        size_t comma = data.find(',');
        if (comma == std::string::npos) throw std::runtime_error("bad image data");
        const char* b64 = data.c_str() + comma + 1;
        DWORD size = 0;
        CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr);
        std::string bytes(size, '\0');
        if (!size || !CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, (BYTE*)bytes.data(), &size, nullptr, nullptr))
            throw std::runtime_error("bad image data");
        if (!WriteFileAtomic(thumbsDir_ + L"\\" + Wide(id) + L".jpg", bytes)) throw std::runtime_error("Couldn't save thumbnail");
        return "https://thumbs.wallpaperplus/" + id + ".jpg";
    }
    if (cmd == "deleteThumb") {
        std::string id = args.at("id");
        if (!id.empty() && id.size() <= 40 &&
            std::all_of(id.begin(), id.end(), [](char c) { return isalnum((unsigned char)c); }))
            DeleteFileW((thumbsDir_ + L"\\" + Wide(id) + L".jpg").c_str());
        return true;
    }
    if (cmd == "showInExplorer") {
        std::wstring path = Wide(args.at("path").get<std::string>());
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
        }
        return true;
    }
    if (cmd == "runningApps") return RunningApps();
    if (cmd == "pickExe") {
        auto picked = OpenDialog(hwnd_, false, false, L"Programs", L"*.exe");
        if (picked.empty()) return nullptr;
        return Utf8(Lower(fs::path(picked[0]).filename().wstring()));
    }
    if (cmd == "windowState") return WindowState(hwnd_);
    if (cmd == "window") {
        std::string action = args.value("action", "");
        if (action == "minimize") PostMessageW(hwnd_, WM_APP_WINDOW, kMinimize, 0);
        if (action == "maximize") PostMessageW(hwnd_, WM_APP_WINDOW, kToggleMaximize, 0);
        if (action == "close") PostMessageW(hwnd_, WM_APP_WINDOW, kClose, 0);
        if (action == "resize") {
            // The top edge sits under the page, so Windows can't offer a resize there by itself.
            // The page reports the press; handing Windows a press on that frame edge starts its
            // normal resize, which then follows the mouse until the button is released.
            static const std::map<std::string, WPARAM> edges = {
                {"top", HTTOP}, {"topleft", HTTOPLEFT}, {"topright", HTTOPRIGHT}};
            auto edge = edges.find(args.value("edge", ""));
            if (edge != edges.end() && !IsZoomed(hwnd_)) {
                POINT pt;
                GetCursorPos(&pt);
                ReleaseCapture();
                PostMessageW(hwnd_, WM_NCLBUTTONDOWN, edge->second, MAKELPARAM((short)pt.x, (short)pt.y));
            }
        }
        return nullptr;
    }
    if (cmd == "openExternal") {  // "Open in browser" in the Browse tab's menu
        std::string url = args.at("url");
        if (url.rfind("https://motionbgs.com/", 0) != 0) throw std::runtime_error("That link can't be opened");
        ShellExecuteW(nullptr, L"open", Wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (cmd == "playerRunning") return FindWindowW(kControllerClass, nullptr) != nullptr;
    if (cmd == "decoderStatus") {  // what the player says each monitor is decoding with
        json out = json::array();
        if (!FindWindowW(kControllerClass, nullptr)) return out;  // a stale file from a crash means nothing
        std::ifstream f(dataDir_ + L"\\player-status.txt", std::ios::binary);
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::vector<std::string> parts;
            for (size_t start = 0;;) {
                size_t tab = line.find('\t', start);
                parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                if (tab == std::string::npos) break;
                start = tab + 1;
            }
            if (parts.size() < 5) continue;
            out.push_back({{"monitor", atoi(parts[0].c_str())},
                           {"device", parts[1]},
                           {"hardware", parts[2] == "hardware"},
                           {"codec", parts[3]},
                           {"size", parts[4]}});
        }
        return out;
    }
    if (cmd == "startPlayer") {
        std::wstring exe = exeDir_ + L"\\WallpaperPlus.exe";
        return (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, exeDir_.c_str(), SW_SHOWNORMAL) > 32;
    }
    if (cmd == "quitPlayer") {
        if (HWND player = FindWindowW(kControllerClass, nullptr)) PostMessageW(player, WM_CLOSE, 0, 0);
        return true;
    }
    throw std::runtime_error("unknown command: " + cmd);
}

// ---------- browse (motionbgs.com) ----------
//
// web.page     {path}          -> {status, url, html}   a listing or search page, as text
// web.media    {path, group, urgent} -> "https://webcache.wallpaperplus/<file>"  thumbnail or preview clip
// web.cancel   {group}         answers "cancelled" to that group's media requests not yet started
// web.download {id, quality}   -> {path}  the full wallpaper, saved into Videos\Wallpaper Plus; progress
//                              arrives meanwhile as {event: "download", data: {id, received, total}}
// web.cancelDownload {id}
void Bridge::HandleWeb(const json& id, const std::string& cmd, const json& args) {
    // Replies from worker threads travel through the window's message queue (see Deliver).
    auto post = [hwnd = hwnd_](const json& message) {
        auto* text = new std::string(message.dump(-1, ' ', false, json::error_handler_t::replace));
        if (!PostMessageW(hwnd, WM_APP_BRIDGE, 0, reinterpret_cast<LPARAM>(text))) delete text;
    };
    auto fail = [post, id](const std::string& error) { post({{"id", id}, {"error", error}}); };

    if (cmd == "web.page") {
        std::string path = args.at("path");
        static const std::regex kPage(R"(^/[A-Za-z0-9/:._%?=&+!'()*~-]{0,300}$)");
        if (!std::regex_match(path, kPage)) throw std::runtime_error("bad page path");
        fetches_.Push(0, "page", [path, post, fail, id](bool cancelled) {
            if (cancelled) return fail("cancelled");
            try {
                std::string html;
                auto res = web::Get(Wide(path), [&](const char* data, size_t n) {
                    html.append(data, n);
                    return html.size() < (8u << 20);
                });
                std::string url = Utf8(res.finalUrl);
                const std::string site = "https://motionbgs.com";
                if (url.rfind(site, 0) == 0) url = url.substr(site.size());
                post({{"id", id}, {"result", {{"status", res.status}, {"url", url}, {"html", html}}}});
            } catch (const std::exception& e) {
                fail(e.what());
            }
        });
        return;
    }

    if (cmd == "web.media") {
        std::string path = args.at("path"), group = args.value("group", "");
        static const std::regex kMedia(R"(^/(i/c/\d+x\d+/)?media/\d+/[A-Za-z0-9._-]+\.(jpg|mp4)$)");
        if (!std::regex_match(path, kMedia)) throw std::runtime_error("bad media path");
        std::wstring name = CacheName(path), file = webCacheDir_ + L"\\" + name;
        std::string url = "https://webcache.wallpaperplus/" + Utf8(name);
        std::error_code ec;
        if (fs::file_size(file, ec) > 0 && !ec) return Send({{"id", id}, {"result", url}});
        const bool clip = path.size() > 4 && path.compare(path.size() - 4, 4, ".mp4") == 0;
        const int priority = args.value("urgent", false) ? 0 : clip ? 2 : 1;  // urgent: the clip being hovered
        fetches_.Push(priority, group, [path, file, url, post, fail, id](bool cancelled) {
            if (cancelled) return fail("cancelled");
            std::wstring part = file + L"." + std::to_wstring(GetCurrentThreadId()) + L".part";
            try {
                web::Response res;
                {
                    std::ofstream out(part, std::ios::binary | std::ios::trunc);
                    res = web::Get(Wide(path), [&](const char* data, size_t n) {
                        out.write(data, (std::streamsize)n);
                        return (bool)out;
                    });
                    if (!out) throw std::runtime_error("cache write failed");
                }
                if (res.status != 200) throw std::runtime_error("not available");
                if (!MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) throw std::runtime_error("cache write failed");
                post({{"id", id}, {"result", url}});
            } catch (const std::exception& e) {
                DeleteFileW(part.c_str());
                fail(e.what());
            }
        });
        return;
    }

    if (cmd == "web.cancel") {
        fetches_.CancelGroup(args.value("group", ""));
        return Send({{"id", id}, {"result", true}});
    }

    if (cmd == "web.download") {
        std::string wallpaper = args.at("id"), quality = args.value("quality", "hd");
        if (wallpaper.empty() || wallpaper.size() > 12 ||
            !std::all_of(wallpaper.begin(), wallpaper.end(), [](char c) { return c >= '0' && c <= '9'; }))
            throw std::runtime_error("bad wallpaper id");
        if (quality != "hd" && quality != "4k") throw std::runtime_error("bad quality");
        std::wstring folder = VideosFolder();
        if (folder.empty()) throw std::runtime_error("Couldn't find your Videos folder");
        folder += L"\\Wallpaper Plus";
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        downloadCancel_[wallpaper] = cancel;
        downloads_.Push(0, "dl:" + wallpaper, [=](bool cancelled) {
            if (cancelled || *cancel) return fail("cancelled");
            CreateDirectoryW(folder.c_str(), nullptr);
            std::wstring part = folder + L"\\" + Wide(wallpaper) + L"-" + Wide(quality) + L".part";
            try {
                uint64_t received = 0, total = 0;
                ULONGLONG lastReport = 0;
                web::Response res;
                {
                    std::ofstream out(part, std::ios::binary | std::ios::trunc);
                    res = web::Get(
                        Wide("/dl/" + quality + "/" + wallpaper + "/"),
                        [&](const char* data, size_t n) {
                            out.write(data, (std::streamsize)n);
                            received += n;
                            ULONGLONG now = GetTickCount64();
                            if (now - lastReport >= 200) {  // a few updates a second is plenty for a bar
                                lastReport = now;
                                post({{"event", "download"},
                                      {"data", {{"id", wallpaper}, {"received", received}, {"total", total}}}});
                            }
                            return (bool)out && !*cancel;
                        },
                        [&](const web::Response& head) { total = head.length; });
                    if (!out) throw std::runtime_error("Couldn't write the file");
                }
                if (*cancel) throw std::runtime_error("cancelled");
                if (res.status != 200) throw std::runtime_error("The server didn't send the file (error " + std::to_string(res.status) + ")");
                if (res.length && received != res.length) throw std::runtime_error("The download was cut short");
                std::wstring name = SafeFileName(res.filename);
                if (name.size() < 5 || !IsVideo(name)) name = L"wallpaper-" + Wide(wallpaper) + L"-" + Wide(quality) + L".mp4";
                std::wstring target = folder + L"\\" + name;
                if (!MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
                    throw std::runtime_error("Couldn't save the file");
                post({{"id", id}, {"result", {{"path", Utf8(target)}}}});
            } catch (const std::exception& e) {
                DeleteFileW(part.c_str());
                fail(e.what());
            }
        });
        return;
    }

    if (cmd == "web.cancelDownload") {
        std::string wallpaper = args.at("id");
        if (auto it = downloadCancel_.find(wallpaper); it != downloadCancel_.end()) *it->second = true;
        downloads_.CancelGroup("dl:" + wallpaper);
        return Send({{"id", id}, {"result", true}});
    }

    throw std::runtime_error("unknown command: " + cmd);
}
