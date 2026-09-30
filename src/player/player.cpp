#include "player.h"
#include "log.h"

#include <codecapi.h>
#include <mfidl.h>
#include <propvarutil.h>
#include <algorithm>
#include <cstring>

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

Player::Player(GpuSet& gpus, HMONITOR monitor, HWND surface, int width, int height, PlaybackSettings settings,
               HWND controller, int number)
    : gpus_(gpus), monitor_(monitor), surface_(surface), width_(width), height_(height), controller_(controller), number_(number) {
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

std::wstring Player::Status() {
    std::lock_guard lock(pendingMutex_);
    return status_;
}

static const wchar_t* CodecName(const GUID& codec) {
    if (codec == MFVideoFormat_H264) return L"H.264";
    if (codec == MFVideoFormat_HEVC) return L"HEVC";
    if (codec == MFVideoFormat_VP90) return L"VP9";
    if (codec == MFVideoFormat_AV1) return L"AV1";
    if (codec == MFVideoFormat_MPEG2) return L"MPEG-2";
    if (codec == MFVideoFormat_WVC1) return L"VC-1";
    return L"other";
}

void Player::SetStatus(bool hardwareFrames) {
    wchar_t text[256];
    swprintf_s(text, L"%s\t%s\t%s%s\t%ux%u", gpu_->name.c_str(), hardwareFrames ? L"hardware" : L"processor",
               CodecName(info_.codec), fmt_.subtype == MFVideoFormat_P010 ? L" 10-bit" : L"",
               fmt_.aperture.right - fmt_.aperture.left, fmt_.aperture.bottom - fmt_.aperture.top);
    {
        std::lock_guard lock(pendingMutex_);
        if (status_ == text) return;
        status_ = text;
    }
    PostMessageW(controller_, WM_APP_STATUS, 0, 0);
}

void Player::ClearStatus() {
    {
        std::lock_guard lock(pendingMutex_);
        if (status_.empty()) return;
        status_.clear();
    }
    PostMessageW(controller_, WM_APP_STATUS, 0, 0);
}

bool Player::UseGpu(Gpu* gpu) {
    if (gpu == gpu_ && swap_) return true;
    reader_.Reset();
    ReleaseOutput();
    if (swap_) {
        swap_.Reset();
        gpu_->context->Flush();  // the old swap chain must be gone before the window gets a new one
    }
    gpu_ = gpu;
    return CreateSwapChain();
}

void Player::ReleaseOutput() {
    inViews_.clear();
    outView_.Reset();
    vp_.Reset();
    vpEnum_.Reset();
    for (auto& p : planes_) p.Reset();
    for (auto& v : planeViews_) v.Reset();
    target_.Reset();
    constants_.Reset();
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
    HRESULT hr = gpu_->factory->CreateSwapChainForHwnd(gpu_->device.Get(), surface_, &d, nullptr, nullptr, &swap_);
    if (FAILED(hr)) {
        Log(L"Monitor %d: can't draw with %s (swap chain 0x%08X)", number_, gpu_->name.c_str(), hr);
        return false;
    }
    gpu_->factory->MakeWindowAssociation(surface_, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    return true;
}

// Reads codec, size and bit depth from the file, to pick a decoder before creating one.
static bool ProbeVideo(const std::wstring& path, VideoInfo& info, HRESULT& hr) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return false;
    ComPtr<IMFMediaType> type;
    if (FAILED(hr = reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &type))) return false;
    info = {};
    type->GetGUID(MF_MT_SUBTYPE, &info.codec);
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &info.width, &info.height);
    const UINT32 profile = MFGetAttributeUINT32(type.Get(), MF_MT_VIDEO_PROFILE, 0);
    const UINT32 transfer = MFGetAttributeUINT32(type.Get(), MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_Unknown);
    info.tenBit = transfer == MFVideoTransFunc_2084 || transfer == MFVideoTransFunc_HLG ||
                  (info.codec == MFVideoFormat_H264 && profile == eAVEncH264VProfile_High10) ||
                  (info.codec == MFVideoFormat_HEVC && profile == eAVEncH265VProfile_Main_420_10) ||
                  (info.codec == MFVideoFormat_VP90 && profile == eAVEncVP9VProfile_420_10);
    return true;
}

bool Player::OpenVideo(const std::wstring& path) {
    reader_.Reset();
    HRESULT hr = S_OK;
    if (!ProbeVideo(path, info_, hr)) {
        Log(L"Monitor %d: can't open %s (0x%08X)", number_, path.c_str(), hr);
        return false;
    }
    decoderCursor_ = 0;
    triedProcessor_ = false;
    retryHardware_ = false;
    if (OpenNextDecoder(path)) return true;
    if (deviceLost_) return false;
    Log(L"Monitor %d: nothing on this PC can decode %s (%s). HEVC/AV1 need the Microsoft Store codec extensions.",
        number_, path.c_str(), CodecName(info_.codec));
    return false;
}

// Tries the remaining decoders for the current video in order: each GPU that says it handles the
// codec, then the processor. With `bestOnly`, only the first GPU, then the processor (right away,
// or when that GPU fails later on).
bool Player::OpenNextDecoder(const std::wstring& path, bool bestOnly) {
    for (;;) {
        Gpu* gpu = gpus_.NextDecoder(info_, monitor_, decoderCursor_);
        const bool hardware = gpu != nullptr;
        if (hardware && bestOnly) decoderCursor_ = SIZE_MAX;
        if (!gpu) {
            if (triedProcessor_) return false;
            triedProcessor_ = true;
            gpu = gpus_.RendererFor(monitor_);
            if (!gpu) return false;
        }
        if (UseGpu(gpu) && OpenReader(path, hardware) && ReadFormat() && ConfigureOutput()) {
            shownKind_ = hardware ? 1 : 2;  // what's expected; the first frame corrects it if needed
            SetStatus(hardware);
            Log(L"Monitor %d: playing %s  %s %ux%u @ %.2f fps, %s, %s %s", number_, path.c_str(),
                CodecName(info_.codec), fmt_.width, fmt_.height, (double)fmt_.fpsN / fmt_.fpsD,
                fmt_.subtype == MFVideoFormat_P010 ? L"10-bit" : L"8-bit",
                hardware ? L"hardware decoder of" : L"processor decoding, drawn by", gpu->name.c_str());
            return true;
        }
        reader_.Reset();
        if (DeviceLost()) return false;
        if (hardware) Log(L"Monitor %d: %s's decoder won't take this video; trying the next option", number_, gpu->name.c_str());
    }
}

bool Player::DeviceLost(HRESULT hr) {
    if (deviceLost_) return true;
    if (!gpu_ || (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET && !gpu_->Removed())) return false;
    Log(L"Monitor %d: GPU device lost (0x%08X)", number_, gpu_->device->GetDeviceRemovedReason());
    deviceLost_ = true;
    PostMessageW(controller_, WM_APP_DEVICE_LOST, 0, 0);
    return true;
}

bool Player::FallBack() {
    if (cur_.monitor.videos.empty()) return false;
    const std::wstring& path = cur_.monitor.videos[index_];
    Log(L"Monitor %d: %s stopped decoding %s; switching decoder", number_,
        hardware_ ? gpu_->name.c_str() : L"the processor", path.c_str());
    const bool wasHardware = hardware_;
    reader_.Reset();
    if (!OpenNextDecoder(path)) return false;
    if (wasHardware && !hardware_) retryHardware_ = true;
    const LONGLONG played = loopBase_;
    ResetClock();
    loopBase_ = played;  // keeps the playlist's rotation timer going
    return true;
}

bool Player::RetryHardware() {
    const std::wstring& path = cur_.monitor.videos[index_];
    Log(L"Monitor %d: trying hardware decoding of %s again", number_, path.c_str());
    reader_.Reset();
    decoderCursor_ = 0;
    triedProcessor_ = false;
    if (!OpenNextDecoder(path, true)) return false;
    retryHardware_ = !hardware_;  // still on the processor: again at the next loop
    return true;
}

bool Player::OpenReader(const std::wstring& path, bool hardware) {
    reader_.Reset();
    hardware_ = hardware;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 3);
    if (hardware) {
        attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu_->dxgiManager.Get());
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    } else {
        // Software decoder; lets Media Foundation convert odd decoder outputs (e.g. I420) to NV12.
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }

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
        Log(L"Monitor %d: no %s decoder for %s (0x%08X)", number_, hardware ? L"hardware" : L"software", path.c_str(), hr);
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

    PROPVARIANT dur;
    PropVariantInit(&dur);
    duration_ = 0;
    if (SUCCEEDED(reader_->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur)) &&
        dur.vt == VT_UI8)
        duration_ = (LONGLONG)dur.uhVal.QuadPart;
    PropVariantClear(&dur);
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
    const UINT32 bytesPerSample = f.subtype == MFVideoFormat_P010 ? 2 : 1;
    f.stride = (LONG)MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, f.width * bytesPerSample);
    if (f.stride < (LONG)(f.width * bytesPerSample)) f.stride = f.width * bytesPerSample;

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

bool Player::ConfigureOutput() {
    ReleaseOutput();
    // A hardware decoder hands over GPU textures for the video processor. Processor-decoded frames
    // (and, rarely, a "hardware" reader that Windows quietly gave a software decoder) go through
    // the shader, so that's always set up.
    if (hardware_ && !ConfigureProcessor()) return false;
    return ConfigureShader();
}

// YUV -> RGB rows for the shader, including range expansion and dimming. Samples arrive as UNORM
// floats: 8-bit as code/255, P010 (10 bits in the top of 16) as code*64/65535.
static void ColorMatrix(UINT32 matrix, UINT32 range, UINT32 height, bool tenBit, float brightness, float out[12]) {
    double kr = 0.2126, kb = 0.0722;  // BT.709
    if (matrix == MFVideoTransferMatrix_BT2020_10 || matrix == MFVideoTransferMatrix_BT2020_12) {
        kr = 0.2627;
        kb = 0.0593;
    } else if (matrix == MFVideoTransferMatrix_BT601 || (matrix == MFVideoTransferMatrix_Unknown && height < 720)) {
        kr = 0.299;
        kb = 0.114;
    }
    const double kg = 1 - kr - kb;
    const int bits = tenBit ? 10 : 8;
    const double toCode = tenBit ? 65535.0 / 64 : 255.0, maxCode = (1 << bits) - 1, shift = 1 << (bits - 8);
    const bool full = range == MFNominalRange_0_255;
    const double yOff = full ? 0 : 16 * shift, yRange = full ? maxCode : 219 * shift;
    const double cOff = 128 * shift, cRange = full ? maxCode : 224 * shift;
    // Y' = ay*y + by; Cb = ac*u + bc; Cr = ac*v + bc
    const double ay = toCode / yRange, by = -yOff / yRange, ac = toCode / cRange, bc = -cOff / cRange;
    const double rv = 2 * (1 - kr), gu = -2 * kb * (1 - kb) / kg, gv = -2 * kr * (1 - kr) / kg, bu = 2 * (1 - kb);
    const double rows[3][4] = {{ay, 0, rv * ac, by + rv * bc},
                               {ay, gu * ac, gv * ac, by + (gu + gv) * bc},
                               {ay, bu * ac, 0, by + bu * bc}};
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = (float)(rows[r][c] * brightness);
}

bool Player::ConfigureShader() {
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swap_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (SUCCEEDED(hr)) hr = gpu_->device->CreateRenderTargetView(backBuffer.Get(), nullptr, &target_);
    if (FAILED(hr)) {
        Log(L"Monitor %d: render target creation failed 0x%08X", number_, hr);
        return false;
    }

    RECT src, dst;
    ComputeRects(src, dst);
    viewport_ = {(float)dst.left, (float)dst.top, (float)(dst.right - dst.left), (float)(dst.bottom - dst.top), 0, 1};
    struct {
        float uvRect[4];
        float matrix[12];
    } constants{};
    constants.uvRect[0] = (float)src.left / fmt_.width;
    constants.uvRect[1] = (float)src.top / fmt_.height;
    constants.uvRect[2] = (float)src.right / fmt_.width;
    constants.uvRect[3] = (float)src.bottom / fmt_.height;
    ColorMatrix(fmt_.matrix, fmt_.range, fmt_.height, fmt_.subtype == MFVideoFormat_P010, cur_.monitor.brightness / 100.f,
                constants.matrix);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(constants);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init{&constants};
    if (FAILED(hr = gpu_->device->CreateBuffer(&bd, &init, &constants_))) {
        Log(L"Monitor %d: constant buffer creation failed 0x%08X", number_, hr);
        return false;
    }
    return true;  // plane textures are made when the first frame in memory arrives
}

bool Player::EnsurePlanes() {
    if (planes_[0]) return true;
    const bool tenBit = fmt_.subtype == MFVideoFormat_P010;
    const DXGI_FORMAT formats[2] = {tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM,
                                    tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM};
    for (int i = 0; i < 2; ++i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = i ? (fmt_.width + 1) / 2 : fmt_.width;
        td.Height = i ? (fmt_.height + 1) / 2 : fmt_.height;
        td.MipLevels = td.ArraySize = 1;
        td.Format = formats[i];
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = gpu_->device->CreateTexture2D(&td, nullptr, &planes_[i]);
        if (SUCCEEDED(hr)) hr = gpu_->device->CreateShaderResourceView(planes_[i].Get(), nullptr, &planeViews_[i]);
        if (FAILED(hr)) {
            Log(L"Monitor %d: frame texture creation failed 0x%08X", number_, hr);
            planes_[0].Reset();
            return false;
        }
    }
    return true;
}

bool Player::ConfigureProcessor() {
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {fmt_.fpsN, fmt_.fpsD};
    cd.InputWidth = fmt_.width;
    cd.InputHeight = fmt_.height;
    cd.OutputFrameRate = {fmt_.fpsN, fmt_.fpsD};
    cd.OutputWidth = width_;
    cd.OutputHeight = height_;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    HRESULT hr = gpu_->videoDevice->CreateVideoProcessorEnumerator(&cd, &vpEnum_);
    if (SUCCEEDED(hr)) hr = gpu_->videoDevice->CreateVideoProcessor(vpEnum_.Get(), 0, &vp_);
    if (FAILED(hr)) {
        Log(L"Monitor %d: video processor creation failed 0x%08X", number_, hr);
        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    swap_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    hr = gpu_->videoDevice->CreateVideoProcessorOutputView(backBuffer.Get(), vpEnum_.Get(), &ovd, &outView_);
    if (FAILED(hr)) {
        Log(L"Monitor %d: output view creation failed 0x%08X", number_, hr);
        return false;
    }

    auto* ctx = gpu_->videoContext.Get();
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
    if (SUCCEEDED(gpu_->videoContext.As(&ctx1)) && SUCCEEDED(vpEnum_.As(&enum1)) &&
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
    HRESULT hr = gpu_->videoDevice->CreateVideoProcessorInputView(tex, vpEnum_.Get(), &ivd, &view);
    if (FAILED(hr)) {
        Log(L"Monitor %d: input view creation failed 0x%08X", number_, hr);
        return nullptr;
    }
    inViews_.push_back({tex, slice, view});
    return view.Get();
}

bool Player::BlitGpuFrame(IMFDXGIBuffer* dxgiBuffer) {
    ComPtr<ID3D11Texture2D> tex;
    UINT slice = 0;
    if (!vp_ || FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dxgiBuffer->GetSubresourceIndex(&slice)))
        return false;
    ID3D11VideoProcessorInputView* in = InputViewFor(tex.Get(), slice);
    if (!in) return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = in;
    HRESULT hr = gpu_->videoContext->VideoProcessorBlt(vp_.Get(), outView_.Get(), 0, 1, &stream);
    if (FAILED(hr)) {
        Log(L"Monitor %d: VideoProcessorBlt failed 0x%08X", number_, hr);
        return false;
    }
    return true;
}

// Copies one plane from a decoded frame in memory into its texture.
static bool UploadPlane(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const BYTE* src, LONG pitch, UINT rowBytes,
                        UINT rows) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    auto* dst = static_cast<BYTE*>(m.pData);
    for (UINT y = 0; y < rows; ++y) memcpy(dst + (size_t)y * m.RowPitch, src + (ptrdiff_t)y * pitch, rowBytes);
    ctx->Unmap(tex, 0);
    return true;
}

bool Player::DrawMemoryFrame(IMFSample* sample) {
    if (!EnsurePlanes()) return false;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;

    const UINT bytesPerSample = fmt_.subtype == MFVideoFormat_P010 ? 2 : 1;
    const UINT rows = fmt_.height, chromaRows = (fmt_.height + 1) / 2;
    const UINT rowBytes = fmt_.width * bytesPerSample, chromaRowBytes = (fmt_.width + 1) / 2 * 2 * bytesPerSample;

    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    ComPtr<IMF2DBuffer2> buffer2d;
    bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) &&
                    SUCCEEDED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &length));
    if (!locked2d) {
        if (FAILED(buffer->Lock(&start, nullptr, &length))) return false;
        scan0 = start;
        pitch = fmt_.stride;
    }
    // The UV plane follows the Y plane at the same pitch. Check it all lies inside the buffer.
    const bool fits = pitch >= (LONG)rowBytes &&
                      (size_t)(scan0 - start) + (size_t)pitch * (rows + chromaRows - 1) + chromaRowBytes <= length;
    bool ok = fits && UploadPlane(gpu_->context.Get(), planes_[0].Get(), scan0, pitch, rowBytes, rows) &&
              UploadPlane(gpu_->context.Get(), planes_[1].Get(), scan0 + (size_t)pitch * rows, pitch, chromaRowBytes,
                          chromaRows);
    if (locked2d)
        buffer2d->Unlock2D();
    else
        buffer->Unlock();
    if (!ok) {
        Log(L"Monitor %d: decoded frame has an unexpected layout (pitch %ld, %lu bytes)", number_, pitch, length);
        return false;
    }

    // Several players share this device's context: hold its lock so no other thread's calls land
    // between these state changes and the draw.
    auto* ctx = gpu_->context.Get();
    if (gpu_->lock) gpu_->lock->Enter();
    const float black[4] = {0, 0, 0, 1};
    ctx->ClearRenderTargetView(target_.Get(), black);
    ctx->OMSetRenderTargets(1, target_.GetAddressOf(), nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(gpu_->raster.Get());
    ctx->RSSetViewports(1, &viewport_);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(gpu_->vs.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(gpu_->ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
    ID3D11ShaderResourceView* views[2] = {planeViews_[0].Get(), planeViews_[1].Get()};
    ctx->PSSetShaderResources(0, 2, views);
    ctx->PSSetSamplers(0, 1, gpu_->sampler.GetAddressOf());
    ctx->Draw(4, 0);
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (gpu_->lock) gpu_->lock->Leave();
    return true;
}

Player::Shown Player::Present(IMFSample* sample) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->GetBufferByIndex(0, &buffer))) return Shown::Failed;
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    const bool onGpu = SUCCEEDED(buffer.As(&dxgiBuffer));
    if (!(onGpu ? BlitGpuFrame(dxgiBuffer.Get()) : DrawMemoryFrame(sample)))
        return DeviceLost() ? Shown::DeviceLost : Shown::Failed;

    HRESULT hr = swap_->Present(1, 0);
    if (DeviceLost(hr)) return Shown::DeviceLost;
    const int kind = onGpu ? 1 : 2;
    if (kind != shownKind_) {  // Windows gave the "hardware" reader a software decoder after all
        shownKind_ = kind;
        Log(L"Monitor %d: frames arrive %s; drawing them that way", number_, onGpu ? L"on the GPU" : L"in memory (processor decoding)");
        SetStatus(onGpu);
    }
    return Shown::Ok;
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
        if (OpenVideo(videos[index_])) {
            ResetClock();
            if (!visible_) ShowWindowAsync(surface_, SW_SHOWNOACTIVATE);
            visible_ = true;
            return true;
        }
        if (deviceLost_) return false;  // not this video's fault: the rebuild starts over
        index_ = (index_ + 1) % videos.size();  // skip unplayable entries
    }
    reader_.Reset();
    ClearStatus();
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
    if (deviceLost_) return;  // not this video's fault: the rebuild starts over
    const size_t n = cur_.monitor.videos.size();
    if (n > 1 && ++failStreak_ < n) {  // try the next entry, but give up after one full lap
        NextVideo();
        return;
    }
    Log(L"Monitor %d: nothing in this wallpaper plays; showing the normal wallpaper", number_);
    ClearStatus();
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
            ClearStatus();
            if (visible_) ShowWindowAsync(surface_, SW_HIDE);
            visible_ = false;
            Log(L"Monitor %d: no video set", number_);
        } else {
            OpenCurrent();
        }
    } else if (lookChanged && reader_ && !ConfigureOutput()) {
        DeviceLost();
    }
    haveClock_ = false;  // speed or frame cap may have changed: re-anchor timing at the next frame
    capNext_ = -1;
}

void Player::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadDescription(GetCurrentThread(), L"WallpaperPlus player");

    bool first = true;  // the swap chain is created when the first video picks its GPU

    while (!stop_ && !deviceLost_) {
        std::optional<PlaybackSettings> next;
        {
            std::lock_guard lock(pendingMutex_);
            next.swap(pending_);
        }
        if (next) {
            Apply(*next, first);
            first = false;
            if (deviceLost_) break;
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
            if (DeviceLost(hr)) break;
            if (!FallBack()) SkipBroken();
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
            // On the processor since a hardware decoder failed mid-video: give the GPUs one more try
            // per loop (its fresh reader starts at the beginning, so no seek). The clock carries on.
            if (retryHardware_) {
                if (!RetryHardware()) SkipBroken();
                continue;
            }
            if (!SeekToStart()) {
                if (DeviceLost()) break;
                Log(L"Monitor %d: can't loop (seek failed)", number_);
                reader_.Reset();
            }
            continue;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            if (!ReadFormat() || !ConfigureOutput()) {
                if (DeviceLost()) break;
                if (!FallBack()) SkipBroken();
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

        const Shown shown = Present(sample.Get());
        if (shown == Shown::DeviceLost) break;  // the controller was told and rebuilds everything
        if (shown == Shown::Failed) {  // this decoder's frames can't be drawn: try the next decoder
            if (!FallBack()) SkipBroken();
            continue;
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
    ReleaseOutput();
    reader_.Reset();
    swap_.Reset();

    // Stay parked until asked to stop, so the controller owns the lifetime.
    while (!stop_) WaitForSingleObject(wake_, INFINITE);
    CoUninitialize();
}
