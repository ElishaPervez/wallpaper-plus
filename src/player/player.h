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
// Each video gets the best decoder the machine has for it (see GpuSet): a GPU's hardware decoder
// (NVDEC, Quick Sync, AMD VCN, all through DXVA) when one handles its codec, size and bit depth,
// otherwise the processor. If a decoder fails mid-video, the video restarts on the next one down.
//
// Frame paths:
//  - Hardware: file -> Media Foundation decoder on the GPU -> decoder texture (NV12/P010) -> D3D11
//    video processor (scale + YUV->RGB + dimming) -> swap chain. Nothing touches the processor.
//  - Processor: file -> Media Foundation software decoder -> frame in memory -> copied into two
//    plane textures -> pixel shader (YUV->RGB + dimming, scaled) -> swap chain. Also used on the
//    software renderer, which has no video processor.
// (Handing NV12 straight to DWM via a YUV DirectComposition swap chain was tried and measured:
// DWM composes it anyway behind the desktop icons, so it saved nothing and was removed.)
//
// The swap chain lives on whichever device decodes (hardware) or draws (processor); it's only
// recreated when a video lands on a different GPU. Settings changes are applied in place on the
// player thread, so switching videos normally never flashes the static wallpaper. The thread
// sleeps between frames and blocks completely while paused or idle.
class Player {
public:
    static constexpr UINT WM_APP_DEVICE_LOST = WM_APP + 20;
    static constexpr UINT WM_APP_STATUS = WM_APP + 21;  // Status() changed

    Player(GpuSet& gpus, HMONITOR monitor, HWND surface, int width, int height, PlaybackSettings settings,
           HWND controller, int number);
    ~Player();

    void Update(const PlaybackSettings& settings);
    void SetPaused(bool paused);
    // What's decoding right now, tab-separated: device name, "hardware" or "processor", codec,
    // frame size. Empty while nothing plays.
    std::wstring Status();

private:
    struct Format {
        UINT32 width = 0, height = 0;
        RECT aperture{};  // visible part of the coded frame
        UINT32 parN = 1, parD = 1;
        UINT32 fpsN = 30, fpsD = 1;
        GUID subtype{};
        UINT32 matrix = 0, range = 0, transfer = 0;
        LONG stride = 0;  // bytes per row of processor-decoded frames
    };
    enum class Shown { Ok, Failed, DeviceLost };

    void Run();
    void Apply(const PlaybackSettings& next, bool first);
    bool OpenCurrent();  // opens videos[index_], skipping unplayable entries
    void NextVideo();
    void SkipBroken();   // current video failed: next playlist entry, or give up after a full lap
    void ResetClock();
    bool UseGpu(Gpu* gpu);  // moves the swap chain to this device if it isn't there already
    bool CreateSwapChain();
    bool OpenVideo(const std::wstring& path);
    bool OpenNextDecoder(const std::wstring& path);  // next decoder in line for the current video
    bool FallBack();  // current decoder failed mid-video: restart it on the next decoder
    bool OpenReader(const std::wstring& path, bool hardware);
    bool ReadFormat();
    bool ConfigureOutput();  // everything that depends on format, fit and brightness
    bool ConfigureProcessor();
    bool ConfigureShader();
    void ReleaseOutput();
    void ComputeRects(RECT& src, RECT& dst) const;
    ID3D11VideoProcessorInputView* InputViewFor(ID3D11Texture2D* tex, UINT slice);
    Shown Present(IMFSample* sample);
    bool BlitGpuFrame(IMFDXGIBuffer* buffer);
    bool DrawMemoryFrame(IMFSample* sample);
    bool EnsurePlanes();
    void SetStatus(bool hardwareFrames);
    void ClearStatus();
    bool SleepUntil(LONGLONG target);  // false if woken early (pause, stop, new settings)
    bool SeekToStart();

    GpuSet& gpus_;
    HMONITOR monitor_;
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

    Gpu* gpu_ = nullptr;  // device the swap chain is on
    VideoInfo info_;      // current video, as read from the file
    size_t decoderCursor_ = 0;
    bool triedProcessor_ = false;
    bool hardware_ = false;  // reader_ was given gpu_'s decoder
    int shownKind_ = 0;      // where frames arrive: 1 on the GPU, 2 in memory

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
    // Processor-decoded frames.
    ComPtr<ID3D11Texture2D> planes_[2];  // Y, then interleaved UV
    ComPtr<ID3D11ShaderResourceView> planeViews_[2];
    ComPtr<ID3D11RenderTargetView> target_;
    ComPtr<ID3D11Buffer> constants_;
    D3D11_VIEWPORT viewport_{};
    Format fmt_;
    LONGLONG frameDuration_ = 333333;

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
    std::wstring status_;  // guarded by pendingMutex_
    HANDLE wake_ = nullptr;   // auto-reset: pause/resume/stop/new settings
    HANDLE timer_ = nullptr;  // high-resolution waitable timer for frame pacing
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
