#pragma once
#include <windows.h>
#include <wrl/client.h>
#include <WebView2.h>
#include <map>
#include <string>

#include "json.hpp"

// The page draws its own title bar. Its minimize / maximize / close buttons post this to the
// window (wParam = one of the WindowAction values) rather than acting inside the web callback.
constexpr UINT WM_APP_WINDOW = WM_APP + 1;
enum WindowAction : WPARAM { kMinimize = 1, kToggleMaximize, kClose };

// Connects the web UI to the machine. The page sends {id, cmd, args} with
// chrome.webview.postMessage; the bridge answers {id, result} or {id, error}. Unsolicited
// notifications go out as {event, data}.
//
// All state lives in files next to the exe: wallpaper.ini (read by the player), library.json
// (the video library, owned by the UI) and thumbs\ (thumbnail images captured by the UI).
class Bridge {
public:
    void Attach(HWND hwnd, ICoreWebView2* webview, const std::wstring& exeDir);
    // Tells the page whether the window is maximized (restore glyph, no top resize edge) and
    // focused (inactive windows dim their title bar).
    void SendWindowState();

private:
    HRESULT OnMessage(ICoreWebView2WebMessageReceivedEventArgs* args);
    nlohmann::json Handle(const std::string& cmd, const nlohmann::json& args);
    void Send(const nlohmann::json& message);
    std::string MediaUrl(const std::wstring& path);

    HWND hwnd_ = nullptr;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3_;
    std::wstring exeDir_, configPath_, libraryPath_, thumbsDir_;
};
