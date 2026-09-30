// Loads the built d3d11.dll proxy (path in argv[1]), creates a device and a
// swap chain through it the same way DS3 does (D3D11CreateDevice + factory
// CreateSwapChain), presents a few frames, and exits 0 on success.
// Check DS3_DLSS.log next to the proxy afterwards for the hook lines.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <cstdio>

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: smoke_test <path-to-proxy-d3d11.dll>\n");
    return 2;
  }
  HMODULE proxy = LoadLibraryW(argv[1]);
  if (!proxy) {
    std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
    return 1;
  }
  auto create_device =
      reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(proxy, "D3D11CreateDevice"));
  if (!create_device) return 1;

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  HRESULT hr = create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                             D3D11_SDK_VERSION, &device, nullptr, &ctx);
  std::printf("D3D11CreateDevice: 0x%08lX\n", hr);
  if (FAILED(hr)) return 1;

  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ds3_dlss_smoke";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"smoke", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360,
                            nullptr, nullptr, wc.hInstance, nullptr);

  IDXGIFactory* factory = nullptr;
  hr = CreateDXGIFactory(IID_PPV_ARGS(&factory));
  if (FAILED(hr)) return 1;
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferDesc.Width = 640;
  desc.BufferDesc.Height = 360;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.OutputWindow = hwnd;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* swap_chain = nullptr;
  hr = factory->CreateSwapChain(device, &desc, &swap_chain);
  std::printf("CreateSwapChain: 0x%08lX\n", hr);
  if (FAILED(hr)) return 1;

  ID3D11Texture2D* back_buffer = nullptr;
  swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
  ID3D11RenderTargetView* rtv = nullptr;
  device->CreateRenderTargetView(back_buffer, nullptr, &rtv);
  // Fullscreen triangle: exercises the shader-creation hooks (hashing) and a
  // valid Draw through the draw hook.
  static const char kShader[] =
      "float4 vs(uint id : SV_VertexID) : SV_Position {"
      "  return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0); }"
      "float4 ps() : SV_Target { return float4(0.8, 0.4, 0.1, 1.0); }";
  ID3DBlob* vs_blob = nullptr;
  ID3DBlob* ps_blob = nullptr;
  D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vs_blob, nullptr);
  D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0, &ps_blob, nullptr);
  if (!vs_blob || !ps_blob) return 1;
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs);
  device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps);
  const D3D11_VIEWPORT viewport = {0, 0, 640, 360, 0, 1};

  const float clear[4] = {0.1f, 0.2f, 0.3f, 1.0f};
  // Like DS3: the draw is recorded on a deferred context one frame ahead and
  // executed by the immediate context in the next frame.
  ID3D11DeviceContext* deferred = nullptr;
  device->CreateDeferredContext(0, &deferred);
  ID3D11CommandList* pending = nullptr;
  // A scene-sized dynamic constant buffer at b8, like DS3's cbSceneParam.
  D3D11_BUFFER_DESC cb_desc = {1024, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                               D3D11_CPU_ACCESS_WRITE, 0, 0};
  ID3D11Buffer* scene_cb = nullptr;
  device->CreateBuffer(&cb_desc, nullptr, &scene_cb);
  for (int i = 0; i < 6; ++i) {
    ctx->ClearRenderTargetView(rtv, clear);
    if (pending) {
      ctx->ExecuteCommandList(pending, FALSE);
      pending->Release();
      pending = nullptr;
    }
    deferred->OMSetRenderTargets(1, &rtv, nullptr);
    deferred->RSSetViewports(1, &viewport);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred->VSSetShader(vs, nullptr, 0);
    deferred->PSSetShader(ps, nullptr, 0);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(deferred->Map(scene_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      float* f = static_cast<float*>(mapped.pData);
      for (int k = 0; k < 256; ++k) f[k] = static_cast<float>(k);
      deferred->Unmap(scene_cb, 0);
    }
    deferred->VSSetConstantBuffers(8, 1, &scene_cb);
    deferred->Draw(3, 0);
    deferred->Draw(3, 0);
    deferred->FinishCommandList(FALSE, &pending);
    hr = swap_chain->Present(0, 0);
    if (FAILED(hr)) break;
  }
  if (pending) pending->Release();
  deferred->Release();
  scene_cb->Release();
  std::printf("Present: 0x%08lX (removed reason 0x%08lX)\n", hr, device->GetDeviceRemovedReason());
  hr = swap_chain->ResizeBuffers(0, 800, 450, DXGI_FORMAT_UNKNOWN, 0);
  std::printf("ResizeBuffers: 0x%08lX (expected to fail while the RTV is alive)\n", hr);

  ctx->ClearState();
  vs->Release();
  ps->Release();
  vs_blob->Release();
  ps_blob->Release();
  rtv->Release();
  back_buffer->Release();
  swap_chain->Release();
  factory->Release();
  ctx->Release();
  device->Release();
  DestroyWindow(hwnd);
  std::printf("OK\n");
  return 0;
}
