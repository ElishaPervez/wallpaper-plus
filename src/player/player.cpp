#include "player.h"
#include "log.h"

#include <mfidl.h>
#include <propvarutil.h>
#include <algorithm>

// Monotonic clock in 100 ns units (Media Foundation's time unit).
static LONGLONG Now100ns() {
    static const LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / freq) * 10'000'000 + (c.QuadPart % freq) * 10'000'000 / freq;
}

Player::Player(const Gpu& gpu, HWND surface, int width, int height, PlaybackSettings settings, HWND controller,
               int number)
    : gpu_(gpu), surface_(surface), width_(width), height_(height), controller_(controller), number_(number) {
    pending_ = std::move(settings);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);  // pre-1803 fallback
    thread_ = std::thread(&Player::Run, this);
}

Player::~Player() {
    stop_ = true;
    SetEvent(wake_);
    if (thread_.joinable()) thread_.join();
    CloseHandle(wake_);
    CloseHandle(timer_);
}

void Player::Update(const PlaybackSettings& settings) {
    {
        std::lock_guard lock(pendingMutex_);
        pending_ = settings;
    }
    SetEvent(wake_);
}

void Player::SetPaused(bool paused) {
    if (paused_.exchange(paused) != paused) {
        Log(L"Monitor %d: %s", number_, paused ? L"paused" : L"resumed");
        SetEvent(wake_);
    }
}

bool Player::CreateSwapChain() {
    DXGI_SWAP_CHAIN_DESC1 d{};
    d.Width = width_;
    d.Height = height_;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.Scaling = DXGI_SCALING_STRETCH;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    HRESULT hr = gpu_.factory->CreateSwapChainForHwnd(gpu_.device.Get(), surface_, &d, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        Log(L"Monitor %d: CreateSwapChainForHwnd failed 0x%08X", number_, hr);
        return false;
    }
    gpu_.factory->MakeWindowAssociation(surface_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    return true;
}

bool Player::OpenVideo(const std::wstring& path) {
    reader_.Reset();
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 3);
    attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu_.dxgiManager.Get());
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);

    HRESULT hr = MFCreateSourceReaderFromURL(path.c_str(), attrs.Get(), &reader_);
    if (FAILED(hr)) {
        Log(L"Monitor %d: can't open %s (0x%08X)", number_, path.c_str(), hr);
        return false;
    }
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    // Any GPU surface format gets the decoder inserted into the chain...
    auto trySubtype = [&](const GUID& sub) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, sub);
        return reader_->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t.Get());
    };
    hr = trySubtype(MFVideoFormat_NV12);
    if (FAILED(hr)) hr = trySubtype(MFVideoFormat_P010);
    if (FAILED(hr)) {
        Log(L"Monitor %d: no decoder for %s (0x%08X). HEVC/AV1 need the Microsoft Store codec extensions.", number_,
            path.c_str(), hr);
        reader_.Reset();
        return false;
    }

    // ...but it must be the decoder's *preferred* one. Asking a 10-bit video for NV12 makes Windows
    // silently switch to its software decoder (many CPU threads). So ask the decoder what it
    // naturally produces and switch to that.
    ComPtr<IMFSourceReaderEx> ex;
    ComPtr<IMFTransform> decoder;
    GUID category;
    if (SUCCEEDED(reader_.As(&ex)) &&
        SUCCEEDED(ex->GetTransformForStream((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &category, &decoder))) {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> avail;
            if (FAILED(decoder->GetOutputAvailableType(0, i, &avail))) break;
            GUID sub{};
            avail->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12 || sub == MFVideoFormat_P010) {
                ComPtr<IMFMediaType> current;
                GUID currentSub{};
                if (SUCCEEDED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current)))
                    current->GetGUID(MF_MT_SUBTYPE, &currentSub);
                if (sub != currentSub && FAILED(trySubtype(sub)))
                    Log(L"Monitor %d: couldn't switch to decoder's preferred format", number_);
                break;
            }
        }
    }

    if (!ReadFormat()) {
        reader_.Reset();
        return false;
    }
    PROPVARIANT dur;
    PropVariantInit(&dur);
    duration_ = 0;
    if (SUCCEEDED(reader_->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur)) &&
        dur.vt == VT_UI8)
        duration_ = (LONGLONG)dur.uhVal.QuadPart;
    PropVariantClear(&dur);
    Log(L"Monitor %d: playing %s  %ux%u @ %.2f fps, %s", number_, path.c_str(), fmt_.width, fmt_.height,
        (double)fmt_.fpsN / fmt_.fpsD, fmt_.subtype == MFVideoFormat_P010 ? L"10-bit" : L"8-bit");
    return true;
}

bool Player::ReadFormat() {
    ComPtr<IMFMediaType> type;
    if (FAILED(reader_->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) return false;

    Format f;
    type->GetGUID(MF_MT_SUBTYPE, &f.subtype);
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height);
    f.aperture = {0, 0, (LONG)f.width, (LONG)f.height};
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr)) &&
        area.Area.cx > 0 && area.Area.cy > 0) {
        f.aperture = {area.OffsetX.value, area.OffsetY.value, area.OffsetX.value + area.Area.cx,
                      area.OffsetY.value + area.Area.cy};
    }
    if (FAILED(MFGetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &f.parN, &f.parD)) || !f.parN || !f.parD)
        f.parN = f.parD = 1;
    if (FAILED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &f.fpsN, &f.fpsD)) || !f.fpsN || !f.fpsD) {
        f.fpsN = 30;
        f.fpsD = 1;
    }
    f.matrix = MFGetAttributeUINT32(type.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown);
    f.range = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_Unknown);
    f.transfer = MFGetAttributeUINT32(type.Get(), MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_Unknown);
    if (!f.width || !f.height) return false;

    fmt_ = f;
    frameDuration_ = 10'000'000LL * f.fpsD / f.fpsN;
    return true;
}

void Player::ComputeRects(RECT& src, RECT& dst) const {
    src = fmt_.aperture;
    dst = {0, 0, width_, height_};
    if (cur_.monitor.fit == FitMode::Stretch) return;

    const double srcW = src.right - src.left, srcH = src.bottom - src.top;
    const double par = (double)fmt_.parN / fmt_.parD;
    const double videoAspect = srcW * par / srcH;
    const double screenAspect = (double)width_ / height_;

    if (cur_.monitor.fit == FitMode::Fit) {
        double scale = std::min(width_ / (srcW * par), height_ / srcH);
        LONG w = (LONG)(srcW * par * scale + 0.5), h = (LONG)(srcH * scale + 0.5);
        dst = {(width_ - w) / 2, (height_ - h) / 2, (width_ - w) / 2 + w, (height_ - h) / 2 + h};
    } else if (videoAspect > screenAspect) {  // fill: crop left/right
        LONG visible = (LONG)(srcH * screenAspect / par + 0.5);
        LONG cut = ((LONG)srcW - visible) / 2;
        src.left += cut;
        src.right = src.left + visible;
    } else {  // fill: crop top/bottom
        LONG visible = (LONG)(srcW * par / screenAspect + 0.5);
        LONG cut = ((LONG)srcH - visible) / 2;
        src.top += cut;
        src.bottom = src.top + visible;
    }
}

static DXGI_COLOR_SPACE_TYPE InputColorSpace(UINT32 matrix, UINT32 range, UINT32 transfer, UINT32 height) {
    const bool full = range == MFNominalRange_0_255;
    if (transfer == MFVideoTransFunc_2084) return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;  // HDR10
    if (matrix == MFVideoTransferMatrix_BT2020_10 || matrix == MFVideoTransferMatrix_BT2020_12)
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    bool bt601 = matrix == MFVideoTransferMatrix_BT601 || (matrix == MFVideoTransferMatrix_Unknown && height < 720);
    if (bt601) return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
}

bool Player::ConfigureProcessor() {
    vp_.Reset();
    vpEnum_.Reset();
    outView_.Reset();
    inViews_.clear();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {fmt_.fpsN, fmt_.fpsD};
    cd.InputWidth = fmt_.width;
    cd.InputHeight = fmt_.height;
    cd.OutputFrameRate = {fmt_.fpsN, fmt_.fpsD};
    cd.OutputWidth = width_;
    cd.OutputHeight = height_;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = gpu_.videoDevice->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
    if (SUCCEEDED(hr)) hr = gpu_.videoDevice->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
    if (FAILED(hr)) {
        Log(L"Monitor %d: video processor creation failed 0x%08X", number_, hr);
        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    swap_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = gpu_.videoDevice->CreateVideoProcessorOutputView(backBuffer.Get(), vpEnum_.Get(), &ovd, &outView_);
    if (FAILED(hr)) {
        Log(L"Monitor %d: output view creation failed 0x%08X", number_, hr);
        return false;
    }

    auto* ctx = gpu_.videoContext.Get();
    RECT src, dst, full = {0, 0, width_, height_};
    ComputeRects(src, dst);
    ctx->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    ctx->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);  // no driver "enhancements"
    ctx->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    ctx->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &dst);
    ctx->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &full);
    D3D11_VIDEO_COLOR black{};
    black.RGBA = {0.f, 0.f, 0.f, 1.f};
    ctx->VideoProcessorSetOutputBackgroundColor(vp_.Get(), FALSE, &black);

    // Dimming: blend the video over the black background at the chosen opacity (free on the GPU).
    if (cur_.monitor.brightness < 100) {
        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        vpEnum_->GetVideoProcessorCaps(&caps);
        if (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ALPHA_STREAM)
            ctx->VideoProcessorSetStreamAlpha(vp_.Get(), 0, TRUE, cur_.monitor.brightness / 100.f);
        else
            Log(L"Monitor %d: GPU video processor can't dim; brightness ignored", number_);
    }

    // Tell the processor how to interpret the video's colors. Prefer the modern color-space API
    // (covers BT.2020 and HDR10 tone-mapping) when the driver supports the conversion.
    const DXGI_FORMAT inFormat = fmt_.subtype == MFVideoFormat_P010 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    const DXGI_COLOR_SPACE_TYPE inCs = InputColorSpace(fmt_.matrix, fmt_.range, fmt_.transfer, fmt_.height);
    const DXGI_COLOR_SPACE_TYPE outCs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    ComPtr<ID3D11VideoContext1> ctx1;
    ComPtr<ID3D11VideoProcessorEnumerator1> enum1;
    BOOL supported = FALSE;
    if (SUCCEEDED(gpu_.videoContext.As(&ctx1)) && SUCCEEDED(vpEnum_.As(&enum1)) &&
        SUCCEEDED(enum1->CheckVideoProcessorFormatConversion(inFormat, inCs, DXGI_FORMAT_B8G8R8A8_UNORM, outCs,
                                                             &supported)) &&
        supported) {
        ctx1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0, inCs);
        ctx1->VideoProcessorSetOutputColorSpace1(vp_.Get(), outCs);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
        in.YCbCr_Matrix = (inCs == DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 ||
                           inCs == DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601) ? 0 : 1;
        in.Nominal_Range = fmt_.range == MFNominalRange_0_255 ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                                              : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        out.RGB_Range = 0;  // full range output
        ctx->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        ctx->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
    }
    return true;
}

ID3D11VideoProcessorInputView* Player::InputViewFor(ID3D11Texture2D* tex, UINT slice) {
    for (auto& c : inViews_)
        if (c.tex == tex && c.slice == slice) return c.view.Get();
    if (inViews_.size() >= 64) inViews_.clear();  // decoder pool changed; drop stale views

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice = slice;
    ComPtr<ID3D11VideoProcessorInputView> view;
    HRESULT hr = gpu_.videoDevice->CreateVideoProcessorInputView(tex, vpEnum_.Get(), &ivd, &view);
    if (FAILED(hr)) {
        if (!bltErrorLogged_) Log(L"Monitor %d: input view creation failed 0x%08X", number_, hr);
        bltErrorLogged_ = true;
        return nullptr;
    }
    inViews_.push_back({tex, slice, view});
    return view.Get();
}

bool Player::Present(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    ComPtr<ID3D11Texture2D> tex;
    UINT slice = 0;
    if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgiBuffer)) ||
        FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dxgiBuffer->GetSubresourceIndex(&slice))) {
        if (!bltErrorLogged_) Log(L"Monitor %d: decoded frame is not on the GPU", number_);
        bltErrorLogged_ = true;
        return true;
    }

    ID3D11VideoProcessorInputView* in = InputViewFor(tex.Get(), slice);
    if (!in) return true;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in;
    HRESULT hr = gpu_.videoContext->VideoProcessorBlt(vp_.Get(), outView_.Get(), 0, 1, &stream);
    if (FAILED(hr) && !bltErrorLogged_) {
        Log(L"Monitor %d: VideoProcessorBlt failed 0x%08X", number_, hr);
        bltErrorLogged_ = true;
    }

    hr = swap_->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Log(L"Monitor %d: GPU device lost (0x%08X)", number_, gpu_.device->GetDeviceRemovedReason());
        return false;
    }
    return true;
}

bool Player::SleepUntil(LONGLONG target) {
    LONGLONG wait = target - Now100ns();
    if (wait <= 0) return true;
    LARGE_INTEGER due;
    due.QuadPart = -wait;  // negative = relative
    SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
    HANDLE handles[] = {timer_, wake_};
    return WaitForMultipleObjects(2, handles, FALSE, INFINITE) == WAIT_OBJECT_0;
}

bool Player::SeekToStart() {
    PROPVARIANT pos;
    InitPropVariantFromInt64(0, &pos);
    HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    return SUCCEEDED(hr);
}

void Player::ResetClock() {
    loopBase_ = 0;
    streamStart_ = -1;
    lastEnd_ = 0;
    capNext_ = -1;
    haveClock_ = false;
    framesThisPass_ = false;
}

bool Player::OpenCurrent() {
    const auto& videos = cur_.monitor.videos;
    for (size_t attempt = 0; attempt < videos.size(); ++attempt) {
        if (OpenVideo(videos[index_]) && ConfigureProcessor()) {
            ResetClock();
            if (!visible_) ShowWindowAsync(surface_, SW_SHOWNOACTIVATE);
            visible_ = true;
            return true;
        }
        index_ = (index_ + 1) % videos.size();  // skip unplayable entries
    }
    reader_.Reset();
    if (visible_) ShowWindowAsync(surface_, SW_HIDE);  // nothing playable: show the normal wallpaper
    visible_ = false;
    return false;
}

void Player::NextVideo() {
    const size_t n = cur_.monitor.videos.size();
    if (n > 1) {
        if (cur_.monitor.shuffle) {
            size_t next = std::uniform_int_distribution<size_t>(0, n - 2)(rng_);
            index_ = next >= index_ ? next + 1 : next;  // any entry except the current one
        } else {
            index_ = (index_ + 1) % n;
        }
    }
    OpenCurrent();
}

void Player::SkipBroken() {
    reader_.Reset();
    const size_t n = cur_.monitor.videos.size();
    if (n > 1 && ++failStreak_ < n) {  // try the next entry, but give up after one full lap
        NextVideo();
        return;
    }
    Log(L"Monitor %d: nothing in this wallpaper plays; showing the normal wallpaper", number_);
    if (visible_) ShowWindowAsync(surface_, SW_HIDE);
    visible_ = false;  // reader_ is empty, so the thread now idles until settings change
}

void Player::Apply(const PlaybackSettings& next, bool first) {
    // The controller sends settings to every monitor on each save; ignore unchanged ones so other
    // monitors don't hitch when one monitor's setting changes.
    if (!first && next == cur_) return;
    const bool videosChanged =
        first || next.monitor.videos != cur_.monitor.videos || next.monitor.shuffle != cur_.monitor.shuffle;
    const bool lookChanged =
        next.monitor.fit != cur_.monitor.fit || next.monitor.brightness != cur_.monitor.brightness;
    cur_ = next;

    if (videosChanged) {
        failStreak_ = 0;
        const size_t n = cur_.monitor.videos.size();
        index_ = n > 1 && cur_.monitor.shuffle ? std::uniform_int_distribution<size_t>(0, n - 1)(rng_) : 0;
        if (n == 0) {
            reader_.Reset();
            if (visible_) ShowWindowAsync(surface_, SW_HIDE);
            visible_ = false;
            Log(L"Monitor %d: no video set", number_);
        } else {
            OpenCurrent();
        }
    } else if (lookChanged && reader_) {
        ConfigureProcessor();
    }
    haveClock_ = false;  // speed or frame cap may have changed: re-anchor timing at the next frame
    capNext_ = -1;
}

void Player::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadDescription(GetCurrentThread(), L"WallpaperPlus player");

    bool ok = CreateSwapChain();
    if (!ok) ShowWindowAsync(surface_, SW_HIDE);
    bool first = true;

    while (ok && !stop_) {
        std::optional<PlaybackSettings> next;
        {
            std::lock_guard lock(pendingMutex_);
            next.swap(pending_);
        }
        if (next) {
            Apply(*next, first);
            first = false;
        }

        if (!reader_ || paused_) {
            WaitForSingleObject(wake_, INFINITE);  // idle: no video, or paused
            haveClock_ = false;                    // resume from the current frame, don't jump ahead
            continue;
        }

        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
        if (FAILED(hr)) {
            Log(L"Monitor %d: decode error 0x%08X", number_, hr);
            SkipBroken();
            continue;
        }

        const double speed = cur_.monitor.speed > 0 ? cur_.monitor.speed : 1.0;
        const LONGLONG rotateAfter = cur_.monitor.videos.size() > 1 && cur_.monitor.rotateMinutes > 0
                                         ? (LONGLONG)cur_.monitor.rotateMinutes * 60 * 10'000'000
                                         : 0;
        auto playedWall = [speed](LONGLONG pos) { return (LONGLONG)(pos / speed); };

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (!framesThisPass_) {
                Log(L"Monitor %d: video has no playable frames", number_);
                SkipBroken();
                continue;
            }
            loopBase_ += lastEnd_ - streamStart_;
            // Playlist: switch at a loop boundary once this video has played long enough.
            if (rotateAfter && playedWall(loopBase_) >= rotateAfter) {
                NextVideo();
                continue;
            }
            streamStart_ = -1;
            framesThisPass_ = false;
            if (!SeekToStart()) {
                Log(L"Monitor %d: can't loop (seek failed)", number_);
                reader_.Reset();
            }
            continue;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            if (!ReadFormat() || !ConfigureProcessor()) {
                reader_.Reset();
                continue;
            }
        }
        if (!sample) continue;

        if (streamStart_ < 0) streamStart_ = ts;
        LONGLONG duration = 0;
        if (FAILED(sample->GetSampleDuration(&duration)) || duration <= 0) duration = frameDuration_;
        lastEnd_ = ts + duration;
        framesThisPass_ = true;

        const LONGLONG pos = loopBase_ + (ts - streamStart_);
        // Videos longer than the rotation interval switch mid-way instead of waiting for the end.
        if (rotateAfter && duration_ > rotateAfter && playedWall(pos) >= rotateAfter) {
            NextVideo();
            continue;
        }

        const LONGLONG now = Now100ns();
        if (!haveClock_) {
            clockBase_ = now - playedWall(pos);
            haveClock_ = true;
            capNext_ = -1;
        }
        const LONGLONG target = clockBase_ + playedWall(pos);

        // Frame-rate cap: show frames on an evenly spaced schedule of `cap` slots per second and skip
        // the rest. Skipped frames are still decoded (later frames depend on them) but never drawn
        // or composited. Comparing against a schedule, not the last shown frame, keeps e.g. a 24 cap
        // on a 30 fps video at 24 instead of collapsing to 15.
        if (cur_.fpsCap > 0) {
            const LONGLONG interval = 10'000'000 / cur_.fpsCap;
            if (capNext_ < 0 || target - capNext_ > interval) capNext_ = target;  // start, or fell behind
            if (target + playedWall(duration) / 2 < capNext_) continue;         // too early for the next slot
            capNext_ += interval;
        }

        if (target > now) {
            if (!SleepUntil(target)) continue;  // woken for pause/stop/settings: drop this frame
        } else if (now - target > 2'500'000) {
            clockBase_ = now - playedWall(pos);  // over 250 ms behind (sleep, hiccup): resync, don't rush
        } else if (now - target > playedWall(duration)) {
            // Already late enough that the next frame is due too (e.g. 2x speed, or a 120 fps video on
            // a 60 Hz screen): skip drawing this one so playback keeps the right pace.
            continue;
        }

        if (!Present(sample.Get())) {
            PostMessageW(controller_, WM_APP_DEVICE_LOST, 0, 0);
            break;
        }
        failStreak_ = 0;

        // Diagnostics (WALLPAPERPLUS_STATS=1): frames actually drawn per 10 s.
        if (statsEnabled_) {
            ++framesShown_;
            if (now - statsSince_ >= 100'000'000) {
                if (statsSince_) Log(L"Monitor %d: %u frames shown in the last 10 s", number_, framesShown_);
                framesShown_ = 0;
                statsSince_ = now;
            }
        }
    }

    // Release GPU objects on this thread before the swap chain's window can go away.
    inViews_.clear();
    outView_.Reset();
    vp_.Reset();
    vpEnum_.Reset();
    reader_.Reset();
    swap_.Reset();

    // Stay parked until asked to stop, so the controller owns the lifetime.
    while (!stop_) WaitForSingleObject(wake_, INFINITE);
    CoUninitialize();
}
