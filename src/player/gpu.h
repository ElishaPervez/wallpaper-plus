#pragma once
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// One D3D11 device shared by every monitor's player and by the Media Foundation decoders.
// Multithread protection is on because each player drives it from its own thread.
struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<IMFDXGIDeviceManager> dxgiManager;
    ComPtr<IDXGIFactory2> factory;

    bool Create();
};
