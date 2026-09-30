// d3d11.dll proxy entry point.
// Loads the real System32\d3d11.dll, resolves the pass-through exports used by
// export_stubs.asm, and intercepts device creation to install our D3D11 hooks.
// Nothing in DarkSoulsIII.exe is touched here.
#include <windows.h>
#include <d3d11.h>

#include <iterator>
#include <string>

#include "common/config.h"
#include "common/log.h"
#include "hooks/d3d11_hooks.h"

// Same order as the STUB indices in export_stubs.asm.
static const char* const kForwardedNames[] = {
    "D3D11CreateDeviceForD3D12",
    "D3DKMTCloseAdapter",
    "D3DKMTDestroyAllocation",
    "D3DKMTDestroyContext",
    "D3DKMTDestroyDevice",
    "D3DKMTDestroySynchronizationObject",
    "D3DKMTPresent",
    "D3DKMTQueryAdapterInfo",
    "D3DKMTSetDisplayPrivateDriverFormat",
    "D3DKMTSignalSynchronizationObject",
    "D3DKMTUnlock",
    "D3DKMTWaitForSynchronizationObject",
    "EnableFeatureLevelUpgrade",
    "OpenAdapter10",
    "OpenAdapter10_2",
    "CreateDirect3D11DeviceFromDXGIDevice",
    "CreateDirect3D11SurfaceFromDXGISurface",
    "D3D11CoreCreateDevice",
    "D3D11CoreCreateLayeredDevice",
    "D3D11CoreGetLayeredDeviceSize",
    "D3D11CoreRegisterLayers",
    "D3D11On12CreateDevice",
    "D3DKMTCreateAllocation",
    "D3DKMTCreateContext",
    "D3DKMTCreateDevice",
    "D3DKMTCreateSynchronizationObject",
    "D3DKMTEscape",
    "D3DKMTGetContextSchedulingPriority",
    "D3DKMTGetDeviceState",
    "D3DKMTGetDisplayModeList",
    "D3DKMTGetMultisampleMethodList",
    "D3DKMTGetRuntimeData",
    "D3DKMTGetSharedPrimaryHandle",
    "D3DKMTLock",
    "D3DKMTOpenAdapterFromHdc",
    "D3DKMTOpenResource",
    "D3DKMTQueryAllocationResidency",
    "D3DKMTQueryResourceInfo",
    "D3DKMTRender",
    "D3DKMTSetAllocationPriority",
    "D3DKMTSetContextSchedulingPriority",
    "D3DKMTSetDisplayMode",
    "D3DKMTSetGammaRamp",
    "D3DKMTSetVidPnSourceOwner",
    "D3DKMTWaitForVerticalBlankEvent",
    "D3DPerformance_BeginEvent",
    "D3DPerformance_EndEvent",
    "D3DPerformance_GetStatus",
    "D3DPerformance_SetMarker",
};

extern "C" void* g_real_procs[std::size(kForwardedNames)] = {};

static HMODULE g_real_d3d11 = nullptr;
static PFN_D3D11_CREATE_DEVICE g_real_create_device = nullptr;
static PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN g_real_create_device_and_swap_chain = nullptr;

static bool LoadRealD3D11() {
  wchar_t sys_dir[MAX_PATH] = {};
  GetSystemDirectoryW(sys_dir, MAX_PATH);
  const std::wstring path = std::wstring(sys_dir) + L"\\d3d11.dll";
  g_real_d3d11 = LoadLibraryW(path.c_str());
  if (!g_real_d3d11) {
    ds3::Log("FATAL: could not load %ls (error %lu)", path.c_str(), GetLastError());
    return false;
  }
  for (size_t i = 0; i < std::size(kForwardedNames); ++i) {
    g_real_procs[i] = reinterpret_cast<void*>(GetProcAddress(g_real_d3d11, kForwardedNames[i]));
    if (!g_real_procs[i]) ds3::Log("warning: real d3d11 has no export %s", kForwardedNames[i]);
  }
  g_real_create_device =
      reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(g_real_d3d11, "D3D11CreateDevice"));
  g_real_create_device_and_swap_chain = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
      GetProcAddress(g_real_d3d11, "D3D11CreateDeviceAndSwapChain"));
  ds3::Log("loaded real d3d11: %ls", path.c_str());
  return g_real_create_device && g_real_create_device_and_swap_chain;
}

extern "C" HRESULT WINAPI Proxy_D3D11CreateDevice(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels, UINT num_feature_levels, UINT sdk_version,
    ID3D11Device** device, D3D_FEATURE_LEVEL* feature_level, ID3D11DeviceContext** context) {
  if (!g_real_create_device) return E_FAIL;
  const HRESULT hr = g_real_create_device(adapter, driver_type, software, flags, feature_levels,
                                          num_feature_levels, sdk_version, device, feature_level,
                                          context);
  ds3::Log("D3D11CreateDevice(flags=0x%X) -> 0x%08X", flags, hr);
  if (SUCCEEDED(hr) && device && *device && ds3::GetConfig().enabled) {
    ds3::hooks::OnDeviceCreated(*device);
  }
  return hr;
}

extern "C" HRESULT WINAPI Proxy_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver_type, HMODULE software, UINT flags,
    const D3D_FEATURE_LEVEL* feature_levels, UINT num_feature_levels, UINT sdk_version,
    const DXGI_SWAP_CHAIN_DESC* swap_chain_desc, IDXGISwapChain** swap_chain,
    ID3D11Device** device, D3D_FEATURE_LEVEL* feature_level, ID3D11DeviceContext** context) {
  if (!g_real_create_device_and_swap_chain) return E_FAIL;
  const HRESULT hr = g_real_create_device_and_swap_chain(
      adapter, driver_type, software, flags, feature_levels, num_feature_levels, sdk_version,
      swap_chain_desc, swap_chain, device, feature_level, context);
  ds3::Log("D3D11CreateDeviceAndSwapChain(flags=0x%X) -> 0x%08X", flags, hr);
  if (SUCCEEDED(hr) && ds3::GetConfig().enabled) {
    if (device && *device) ds3::hooks::OnDeviceCreated(*device);
    if (swap_chain && *swap_chain) ds3::hooks::OnSwapChainCreated(*swap_chain);
  }
  return hr;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    ds3::SetSelfModule(instance);
    ds3::LogInit();
    ds3::Log("DS3_DLSS proxy attached");
    ds3::LoadConfig();
    if (!LoadRealD3D11()) return FALSE;
  }
  return TRUE;
}
