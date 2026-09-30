#pragma once
#include <d3d11.h>

namespace ds3::hooks {

// Installs device/context/DXGI-factory hooks (once; hooks patch the shared
// d3d11/dxgi code, so they cover every device and context instance).
void OnDeviceCreated(ID3D11Device* device);

// Installs Present/ResizeBuffers hooks (once).
void OnSwapChainCreated(IDXGISwapChain* swap_chain);

}  // namespace ds3::hooks
