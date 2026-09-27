#pragma once
#include "config.h"
#include "gpu.h"

#include <mfidl.h>
#include <mfreadwrite.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Everything a player needs to know; the controller sends a new copy whenever settings change.
struct PlaybackSettings {
    MonitorSetting monitor;
    int fpsCap = 0;
    bool operator==(const PlaybackSettings&) const = default;
};

// Plays one monitor's wallpaper (a video or a playlist) into its surface window, on its own thread.
//
// Frame path, all on the GPU: file -> Media Foundation hardware decoder (NVDEC/DXVA) -> decoder
// texture (NV12/P010) -> D3D11 video processor (scale + YUV->RGB + dimming) -> swap chain.
// (Handing NV12 straight to DWM via a YUV DirectComposition swap chain was tried and measured:
// DWM composes it anyway behind the desktop icons, so it saved nothing and was removed.)
//
// Settings changes are applied in place on the player thread: the swap chain and window stay, so
// switching videos never flashes the static wallpaper. The thread sleeps between frames and
// blocks completely while paused or idle.
class Player {
public:
    static constexpr UINT WM_APP_DEVICE_LOST = WM_APP + 20;

    Player(const Gpu& gpu, HWND surface, int width, int height, PlaybackSettings settings, HWND controller,
           int number);
    ~Player();

    void Update(const PlaybackSettings& settings);
    void SetPaused(bool paused);

private:
    struct Format {
        UINT32 width = 0, height = 0;
        RECT aperture{};  // visible part of the coded frame
        UINT32 parN = 1, parD = 1;
        UINT32 fpsN = 30, fpsD = 1;
        GUID subtype{};
        UINT32 matrix = 0, range = 0, transfer = 0;
    };

    void Run();
    void Apply(const PlaybackSettings& next, bool first);
    bool OpenCurrent();  // opens videos[index_], skipping unplayable entries
    void NextVideo();
    void SkipBroken();   // current video failed: next playlist entry, or give up after a full lap
    void ResetClock();
    bool CreateSwapChain();
    bool OpenVideo(const std::wstring& path);
    bool ReadFormat();
    bool ConfigureProcessor();
    void ComputeRects(RECT& src, RECT& dst) const;
    ID3D11VideoProcessorInputView* InputViewFor(ID3D11Texture2D* tex, UINT slice);
    bool Present(IMFSample* sample);   // false only on device loss
    bool SleepUntil(LONGLONG target);  // false if woken early (pause, stop, new settings)
    bool SeekToStart();

    const Gpu& gpu_;
    HWND surface_;
    int width_, height_;
    HWND controller_;
    int number_;

    // Owned by the player thread.
    PlaybackSettings cur_;
    size_t index_ = 0;
    size_t failStreak_ = 0;  // consecutive entries that failed to play
    LONGLONG duration_ = 0;  // current video length, 100 ns units (0 if unknown)
    std::mt19937 rng_{std::random_device{}()};
    bool visible_ = true;

    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<IMFSourceReader> reader_;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    ComPtr<ID3D11VideoProcessorOutputView> outView_;
    struct CachedView {
        ID3D11Texture2D* tex;
        UINT slice;
        ComPtr<ID3D11VideoProcessorInputView> view;
    };
    std::vector<CachedView> inViews_;
    Format fmt_;
    LONGLONG frameDuration_ = 333333;
    bool bltErrorLogged_ = false;

    // Playback clock. Timeline position `pos` (video time, keeps growing across loops) is shown at
    // wall time clockBase_ + pos / speed.
    LONGLONG clockBase_ = 0, loopBase_ = 0, streamStart_ = -1, lastEnd_ = 0, capNext_ = -1;
    bool haveClock_ = false, framesThisPass_ = false;
    const bool statsEnabled_ = GetEnvironmentVariableW(L"WALLPAPERPLUS_STATS", nullptr, 0) > 0;
    unsigned framesShown_ = 0;
    LONGLONG statsSince_ = 0;

    // Shared with the controller thread.
    std::mutex pendingMutex_;
    std::optional<PlaybackSettings> pending_;
    HANDLE wake_ = nullptr;   // auto-reset: pause/resume/stop/new settings
    HANDLE timer_ = nullptr;  // high-resolution waitable timer for frame pacing
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
