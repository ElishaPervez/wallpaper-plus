#pragma once
#include "config.h"

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// What a video needs from a hardware decoder (read from the file before choosing a GPU).
struct VideoInfo {
    GUID codec{};  // MFVideoFormat_H264, _HEVC, _VP90, _AV1, ...
    UINT32 width = 0, height = 0;
    bool tenBit = false;
};

// One graphics device. Multithread protection is on because each player drives it from its own
// thread, and Media Foundation's decoders use it too.
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D10Multithread> lock;             // held around multi-call draw sequences
    ComPtr<ID3D11VideoDevice> videoDevice;      // null on the software renderer
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;   // null if this device can't hardware-decode
    ComPtr<IDXGIFactory2> factory;

    // Draws processor-decoded frames (NV12/P010 planes -> RGB, scaled). Works on every device,
    // including the software renderer, which has no video processor.
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;

    std::wstring name;
    UINT vendor = 0;
    bool software = false;  // Microsoft's software renderer (no usable GPU)

    bool CanHardwareDecode() const { return dxgiManager != nullptr; }
    // True if this GPU's decoder handles the video's codec, size and bit depth. Unknown codecs
    // answer true: Media Foundation gets to try, and the player checks what it actually got.
    bool Decodes(const VideoInfo& v) const;
};

// Every graphics adapter in the machine, ranked for the decoder preference. Devices are created
// the first time a player needs them, so e.g. a laptop's NVIDIA chip stays asleep in power-saving
// mode unless the integrated GPU can't decode a video.
class GpuSet {
public:
    bool Init(DecoderPreference pref);  // false if no device at all could be created
    void Reset();

    // The next hardware decoder that can take this video, best first, starting from `cursor`
    // (which it advances). Null when none is left, or always in "processor only" mode. Lazy, so
    // lower-ranked GPUs are only woken if the better ones can't decode the video.
    Gpu* NextDecoder(const VideoInfo& v, size_t& cursor);
    // Where to draw processor-decoded frames for a monitor: the GPU that drives it if possible,
    // otherwise any GPU, otherwise the software renderer.
    Gpu* RendererFor(HMONITOR monitor);

private:
    struct Slot {
        ComPtr<IDXGIAdapter1> adapter;  // null for the software renderer
        std::wstring name;
        UINT vendor = 0;
        std::vector<HMONITOR> outputs;
        std::unique_ptr<Gpu> gpu;
        bool failed = false;
    };
    Gpu* Open(Slot& s);  // creates the device on first use; caller holds mutex_

    std::mutex mutex_;
    DecoderPreference pref_ = DecoderPreference::Auto;
    std::vector<Slot> slots_;  // hardware adapters in preference order, software renderer last
};
