#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <WebView2.h>
#include <atomic>
#include <map>
#include <memory>
#include <string>

#include "json.hpp"
#include "web.h"

// The page draws its own title bar. Its minimize / maximize / close buttons post this to the
// window (wParam = one of the WindowAction values) rather than acting inside the web callback.
constexpr UINT WM_APP_WINDOW = WM_APP + 1;
enum WindowAction : WPARAM { kMinimize = 1, kToggleMaximize, kClose };
// Background work (Browse tab downloads) finishes on other threads; its message for the page is
// posted here (lParam = heap std::string of JSON) so it's sent from the window's own thread.
constexpr UINT WM_APP_BRIDGE = WM_APP + 2;

// Connects the web UI to the machine. The page sends {id, cmd, args} with
// chrome.webview.postMessage; the bridge answers {id, result} or {id, error}. Unsolicited
// notifications go out as {event, data}.
//
// All state lives in files next to the exe: wallpaper.ini (read by the player), library.json
// (the video library, owned by the UI), thumbs\ (thumbnail images captured by the UI) and
// webcache\ (thumbnails and preview clips from motionbgs.com). Downloaded wallpapers go to
// Videos\Wallpaper Plus.
//
// Commands starting with "web." answer later, from background threads (see HandleWeb).
class Bridge {
public:
    void Attach(HWND hwnd, ICoreWebView2* webview, const std::wstring& exeDir);
    // Tells the page whether the window is maximized (restore glyph, no top resize edge) and
    // focused (inactive windows dim their title bar).
    void SendWindowState();
    // Sends a message a background job posted with WM_APP_BRIDGE.
    void Deliver(LPARAM message);

private:
    HRESULT OnMessage(ICoreWebView2WebMessageReceivedEventArgs* args);
    nlohmann::json Handle(const std::string& cmd, const nlohmann::json& args);
    void HandleWeb(const nlohmann::json& id, const std::string& cmd, const nlohmann::json& args);
    void Send(const nlohmann::json& message);
    std::string MediaUrl(const std::wstring& path);

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3_;
    std::wstring exeDir_, configPath_, libraryPath_, thumbsDir_, webCacheDir_;
    web::WorkQueue fetches_{6};    // pages, thumbnails, preview clips
    web::WorkQueue downloads_{1};  // full wallpapers, one at a time
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> downloadCancel_;  // by wallpaper id
};
