#include "gpu.h"
#include "log.h"

bool Gpu::Create() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) {
        Log(L"GPU: D3D11CreateDevice failed 0x%08X", hr);
        return false;
    }

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (FAILED(device.As(&videoDevice)) || FAILED(context.As(&videoContext))) {
        Log(L"GPU: no video device support");
        return false;
    }

    UINT token = 0;
    if (FAILED(hr = MFCreateDXGIDeviceManager(&token, &dxgiManager)) ||
        FAILED(hr = dxgiManager->ResetDevice(device.Get(), token))) {
        Log(L"GPU: DXGI device manager failed 0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    device.As(&dxgiDevice);
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    // Let games and other apps win when the GPU is busy.
    dxgiDevice->SetGPUThreadPriority(-7);

    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    Log(L"GPU: %s", desc.Description);
    return true;
}
