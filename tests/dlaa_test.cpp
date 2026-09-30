// Drives the DLAA path of the built proxy with a synthetic DS3-like frame:
//   cbSceneParam upload (960 bytes) -> ClearDepthStencilView on a D32S8 depth
//   -> Draw with the game's first post-process pixel shader (393A947C35535471)
//   reading an RGBA16F scene -> Present.
// usage: dlaa_test <proxy d3d11.dll> <393A947C35535471.ps.cso> [mv|edge]
// With "mv" ([DLSS] DebugView=1 in the ini) the scene is replaced by
// |motion vector| * 0.25, and the test checks it against the analytic value.
// With "edge" a slanted white edge is rendered through the (jittered) camera
// constants for 48 static frames, and the result is compared with a
// supersampled reference: the error shows whether [DLSS] JitterSign is right.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

constexpr UINT kW = 640, kH = 360;
constexpr float kDepth = 0.99f;  // cleared scene depth
constexpr float kNear = 0.08f, kProj10 = 1.00001f;
constexpr float kStep = 0.1f;  // camera x movement per frame in the moving phase

void Mul(const double* a, const double* b, double* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
      out[r * 4 + c] = s;
    }
}

bool Invert(const double* m, double* out) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int p = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
    if (std::fabs(a[p][c]) < 1e-12) return false;
    for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[p][k]);
    const double inv = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  return true;
}

double P0() { return 1.0 / (std::tan(30.0 * 3.14159265358979 / 180.0) * kW / kH); }

// cbSceneParam for a camera at (cx, 1.5, -300) looking down +z.
void FillSceneCb(float* cb, double cx) {
  std::memset(cb, 0, 960);
  cb[4] = kW;
  cb[5] = kH;
  cb[6] = 1.0f / kW;
  cb[7] = 1.0f / kH;
  const double cam[3] = {cx, 1.5, -300.0};
  cb[12] = static_cast<float>(cam[0]);
  cb[13] = static_cast<float>(cam[1]);
  cb[14] = static_cast<float>(cam[2]);
  const double p0 = P0(), p5 = p0 * kW / kH;
  const double proj[16] = {p0, 0, 0, 0, 0, p5, 0, 0, 0, 0, kProj10, -kNear * kProj10, 0, 0, 1, 0};
  const double view[16] = {1, 0, 0, -cam[0], 0, 1, 0, -cam[1], 0, 0, 1, -cam[2], 0, 0, 0, 1};
  double vp[16], inv_vp[16], inv_proj[16];
  Mul(proj, view, vp);
  Invert(vp, inv_vp);
  Invert(proj, inv_proj);
  for (int i = 0; i < 16; ++i) {
    cb[32 + i] = static_cast<float>(inv_vp[i]);
    cb[48 + i] = static_cast<float>(inv_proj[i]);
    cb[64 + i] = static_cast<float>(proj[i]);
    cb[96 + i] = static_cast<float>(proj[i]);  // camera-relative view-proj: no rotation
  }
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
  float v = exp == 0 ? std::ldexp(static_cast<float>(mant), -24)
                     : std::ldexp(static_cast<float>(mant | 1024), static_cast<int>(exp) - 25);
  return sign ? -v : v;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: dlaa_test <proxy d3d11.dll> <393A...ps.cso> [mv]\n");
    return 2;
  }
  const bool check_mv = argc > 3 && wcscmp(argv[3], L"mv") == 0;
  const bool edge = argc > 3 && wcscmp(argv[3], L"edge") == 0;
  std::ifstream file(argv[2], std::ios::binary);
  const std::vector<char> ps_code((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  if (ps_code.empty()) return 1;

  HMODULE proxy = LoadLibraryW(argv[1]);
  if (!proxy) return 1;
  auto create_device =
      reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(proxy, "D3D11CreateDevice"));
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  // DS3_DLAA_TEST_DEBUG=1: D3D11 debug layer, messages printed after each frame.
  const bool debug_layer = GetEnvironmentVariableA("DS3_DLAA_TEST_DEBUG", nullptr, 0) != 0;
  HRESULT hr = create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                             debug_layer ? D3D11_CREATE_DEVICE_DEBUG : 0, &level, 1,
                             D3D11_SDK_VERSION, &device, nullptr, &ctx);
  if (FAILED(hr)) return 1;
  ID3D11InfoQueue* info = nullptr;
  if (debug_layer) device->QueryInterface(IID_PPV_ARGS(&info));
  auto print_messages = [info](int frame) {
    if (!info) return;
    for (UINT64 i = 0, n = info->GetNumStoredMessages(); i < n; ++i) {
      SIZE_T len = 0;
      info->GetMessage(i, nullptr, &len);
      std::vector<char> buf(len);
      auto* msg = reinterpret_cast<D3D11_MESSAGE*>(buf.data());
      if (SUCCEEDED(info->GetMessage(i, msg, &len)) &&
          msg->Severity <= D3D11_MESSAGE_SEVERITY_WARNING)
        std::printf("frame %d d3d11: %.*s\n", frame, static_cast<int>(msg->DescriptionByteLength),
                    msg->pDescription);
    }
    info->ClearStoredMessages();
  };

  WNDCLASSW wc = {};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"ds3_dlaa_test";
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"dlaa", WS_OVERLAPPEDWINDOW, 0, 0, kW, kH,
                            nullptr, nullptr, wc.hInstance, nullptr);
  IDXGIFactory* factory = nullptr;
  CreateDXGIFactory(IID_PPV_ARGS(&factory));
  DXGI_SWAP_CHAIN_DESC desc = {};
  desc.BufferDesc.Width = kW;
  desc.BufferDesc.Height = kH;
  desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.OutputWindow = hwnd;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  IDXGISwapChain* swap_chain = nullptr;
  if (FAILED(factory->CreateSwapChain(device, &desc, &swap_chain))) return 1;

  ID3D11PixelShader* ps = nullptr;
  hr = device->CreatePixelShader(ps_code.data(), ps_code.size(), nullptr, &ps);
  std::printf("CreatePixelShader(393A): 0x%08lX\n", hr);
  if (FAILED(hr)) return 1;
  // Fullscreen triangle matching 393A's input signature (SV_Position, TEXCOORD0).
  static const char kVs[] =
      "void main(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {"
      "  uv = float2(id == 1 ? 2.0 : 0.0, id == 2 ? 2.0 : 0.0);"
      "  pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }";
  ID3DBlob* vs_blob = nullptr;
  D3DCompile(kVs, sizeof(kVs) - 1, nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vs_blob,
             nullptr);
  ID3D11VertexShader* vs = nullptr;
  if (!vs_blob || FAILED(device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                                    vs_blob->GetBufferSize(), nullptr, &vs)))
    return 1;
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  ID3D11SamplerState* sampler = nullptr;
  device->CreateSamplerState(&sd, &sampler);

  D3D11_TEXTURE2D_DESC td = {};
  td.Width = kW;
  td.Height = kH;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* scene = nullptr;
  device->CreateTexture2D(&td, nullptr, &scene);
  ID3D11Texture2D* post = nullptr;  // stands in for the game's post-process target
  device->CreateTexture2D(&td, nullptr, &post);
  ID3D11RenderTargetView* post_rtv = nullptr;
  device->CreateRenderTargetView(post, nullptr, &post_rtv);
  ID3D11ShaderResourceView* scene_srv = nullptr;
  ID3D11RenderTargetView* scene_rtv = nullptr;
  device->CreateShaderResourceView(scene, nullptr, &scene_srv);
  device->CreateRenderTargetView(scene, nullptr, &scene_rtv);
  td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;  // like DS3's T0: DSV only
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D* depth = nullptr;
  device->CreateTexture2D(&td, nullptr, &depth);
  D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {};
  dsv_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
  ID3D11DepthStencilView* dsv = nullptr;
  device->CreateDepthStencilView(depth, &dsv_desc, &dsv);
  td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  td.BindFlags = 0;
  td.Usage = D3D11_USAGE_STAGING;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ID3D11Texture2D* staging = nullptr;
  device->CreateTexture2D(&td, nullptr, &staging);
  // Edge scene: everything left of the line through kEdgeA and kEdgeB (NDC) is
  // white, drawn at view depth kEdgeZ through VC_MatrixViewProj (b8, offset 384).
  constexpr double kEdgeA[2] = {0.30, -1.2}, kEdgeB[2] = {0.10, 1.2}, kEdgeZ = 8.0;
  const double edge_ndc[3][2] = {
      {kEdgeA[0] + 10 * (kEdgeA[0] - kEdgeB[0]), kEdgeA[1] + 10 * (kEdgeA[1] - kEdgeB[1])},
      {kEdgeB[0] + 10 * (kEdgeB[0] - kEdgeA[0]), kEdgeB[1] + 10 * (kEdgeB[1] - kEdgeA[1])},
      {-30.0, 0.0}};
  char edge_src[1024];
  std::snprintf(edge_src, sizeof(edge_src),
                "cbuffer cbSceneParam : register(b8) { float4 pad[24]; row_major float4x4 vp; };"
                "static const float3 kPos[3] = { float3(%.9g, %.9g, %.9g), float3(%.9g, %.9g, "
                "%.9g), float3(%.9g, %.9g, %.9g) };"
                "float4 vs(uint id : SV_VertexID) : SV_Position {"
                "  return mul(vp, float4(kPos[id], 1)); }"
                "float4 ps() : SV_Target { return 1; }",
                edge_ndc[0][0] * kEdgeZ / P0(), edge_ndc[0][1] * kEdgeZ / (P0() * kW / kH), kEdgeZ,
                edge_ndc[1][0] * kEdgeZ / P0(), edge_ndc[1][1] * kEdgeZ / (P0() * kW / kH), kEdgeZ,
                edge_ndc[2][0] * kEdgeZ / P0(), edge_ndc[2][1] * kEdgeZ / (P0() * kW / kH), kEdgeZ);
  ID3DBlob* edge_vs_blob = nullptr;
  ID3DBlob* edge_ps_blob = nullptr;
  D3DCompile(edge_src, std::strlen(edge_src), nullptr, nullptr, nullptr, "vs", "vs_5_0", 0, 0,
             &edge_vs_blob, nullptr);
  D3DCompile(edge_src, std::strlen(edge_src), nullptr, nullptr, nullptr, "ps", "ps_5_0", 0, 0,
             &edge_ps_blob, nullptr);
  if (!edge_vs_blob || !edge_ps_blob) return 1;
  ID3D11VertexShader* edge_vs = nullptr;
  ID3D11PixelShader* edge_ps = nullptr;
  device->CreateVertexShader(edge_vs_blob->GetBufferPointer(), edge_vs_blob->GetBufferSize(),
                             nullptr, &edge_vs);
  device->CreatePixelShader(edge_ps_blob->GetBufferPointer(), edge_ps_blob->GetBufferSize(),
                            nullptr, &edge_ps);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  ID3D11RasterizerState* no_cull = nullptr;
  device->CreateRasterizerState(&rd, &no_cull);

  D3D11_BUFFER_DESC cb_desc = {960, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  ID3D11Buffer* scene_cb = nullptr;
  device->CreateBuffer(&cb_desc, nullptr, &scene_cb);

  // Expected |mv.x| in the moving phase for the cleared depth plane.
  const double z = kNear * kProj10 / (kProj10 - kDepth);
  const double expected = P0() * kStep / z * kW / 2 * 0.25;
  int failures = 0;
  double cx = 0.0;
  alignas(16) float cb[240];
  const int frames = edge ? 48 : 12;
  for (int frame = 0; frame < frames; ++frame) {
    if (frame >= 6 && !edge) cx += kStep;
    FillSceneCb(cb, cx);
    ctx->UpdateSubresource(scene_cb, 0, nullptr, cb, 0, 0);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, kDepth, 0);
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(scene_rtv, edge ? black : grey);
    const D3D11_VIEWPORT viewport = {0, 0, kW, kH, 0, 1};
    if (edge) {
      ctx->OMSetRenderTargets(1, &scene_rtv, nullptr);
      ctx->RSSetViewports(1, &viewport);
      ctx->RSSetState(no_cull);
      ctx->VSSetShader(edge_vs, nullptr, 0);
      ctx->VSSetConstantBuffers(8, 1, &scene_cb);
      ctx->PSSetShader(edge_ps, nullptr, 0);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->Draw(3, 0);
    }
    ctx->OMSetRenderTargets(1, &post_rtv, nullptr);
    ctx->RSSetViewports(1, &viewport);
    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetSamplers(2, 1, &sampler);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->CSSetShaderResources(3, 1, &scene_srv);  // a game compute binding that must survive
    ctx->PSSetShaderResources(0, 1, &scene_srv);
    ctx->PSSetConstantBuffers(8, 1, &scene_cb);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
    // The game's state must be intact after our work.
    ID3D11ShaderResourceView* bound = nullptr;
    ctx->PSGetShaderResources(0, 1, &bound);
    if (bound != scene_srv) {
      std::printf("frame %d: game SRV binding lost\n", frame);
      ++failures;
    }
    if (bound) bound->Release();
    ctx->CSGetShaderResources(3, 1, &bound);
    ID3D11VertexShader* bound_vs = nullptr;
    ctx->VSGetShader(&bound_vs, nullptr, nullptr);
    if (bound != scene_srv || bound_vs != vs) {
      std::printf("frame %d: game VS or compute binding lost\n", frame);
      ++failures;
    }
    if (bound) bound->Release();
    if (bound_vs) bound_vs->Release();

    if (edge && frame == frames - 1) {
      ctx->CopyResource(staging, scene);
      D3D11_MAPPED_SUBRESOURCE m = {};
      if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return 1;
      // Reference: coverage of each pixel, 16x16 samples, unjittered.
      const double dx = kEdgeB[0] - kEdgeA[0], dy = kEdgeB[1] - kEdgeA[1];
      auto side = [&](double x, double y) { return dx * (y - kEdgeA[1]) - dy * (x - kEdgeA[0]); };
      const bool inside_sign = side(-1.0, 0.0) > 0;
      double sum = 0, edge_sum = 0;
      int edge_pixels = 0;
      for (UINT y = 0; y < kH; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(m.pData) +
                                                            y * m.RowPitch);
        for (UINT x = 0; x < kW; ++x) {
          int covered = 0;
          for (int sy = 0; sy < 16; ++sy)
            for (int sx = 0; sx < 16; ++sx) {
              const double nx = (x + (sx + 0.5) / 16) / kW * 2 - 1;
              const double ny = 1 - (y + (sy + 0.5) / 16) / kH * 2;
              covered += (side(nx, ny) > 0) == inside_sign;
            }
          const double ref = covered / 256.0;
          const double err = std::fabs(HalfToFloat(row[x * 4]) - ref);
          sum += err;
          if (ref > 0.0 && ref < 1.0) {
            edge_sum += err;
            ++edge_pixels;
          }
        }
      }
      const auto* mid = reinterpret_cast<const uint16_t*>(static_cast<const char*>(m.pData) +
                                                          (kH / 2) * m.RowPitch);
      std::printf("edge: white %.4f, black %.4f\n", HalfToFloat(mid[10 * 4]),
                  HalfToFloat(mid[(kW - 10) * 4]));
      ctx->Unmap(staging, 0);
      std::printf("edge: mean error on %d edge pixels = %.4f, whole image = %.5f\n", edge_pixels,
                  edge_sum / edge_pixels, sum / (kW * kH));
    }
    if (!edge && (frame == 5 || frame == 11)) {
      ctx->CopyResource(staging, scene);
      D3D11_MAPPED_SUBRESOURCE m = {};
      hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
      if (FAILED(hr)) {
        std::printf("frame %d: Map failed 0x%08lX, removed reason 0x%08lX\n", frame, hr,
                    device->GetDeviceRemovedReason());
        return 1;
      }
      const auto* px = reinterpret_cast<const uint16_t*>(static_cast<const char*>(m.pData) +
                                                         (kH / 2) * m.RowPitch) + (kW / 2) * 4;
      const float r = HalfToFloat(px[0]), g = HalfToFloat(px[1]);
      ctx->Unmap(staging, 0);
      bool ok;
      if (check_mv) {
        const double want = frame == 5 ? 0.0 : expected;
        ok = std::fabs(r - want) < 0.01 && std::fabs(g) < 0.01;
        std::printf("frame %2d: |mv|*0.25 = (%.4f, %.4f), expected (%.4f, 0) %s\n", frame, r, g,
                    want, ok ? "ok" : "MISMATCH");
      } else {  // DLAA of a flat grey scene stays grey
        ok = std::fabs(r - 0.5f) < 0.02f && std::fabs(g - 0.5f) < 0.02f;
        std::printf("frame %2d: scene after DLAA = (%.4f, %.4f), expected 0.5 %s\n", frame, r, g,
                    ok ? "ok" : "MISMATCH");
      }
      if (!ok) ++failures;
    }
    swap_chain->Present(0, 0);
    print_messages(frame);
  }
  std::printf("removed reason 0x%08lX\n", device->GetDeviceRemovedReason());
  if (FAILED(device->GetDeviceRemovedReason())) ++failures;
  std::printf(failures ? "FAILED\n" : "OK\n");
  // Released by process exit; the proxy stays loaded like in the game.
  return failures ? 1 : 0;
}
