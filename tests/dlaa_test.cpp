// Drives the DLAA path of the built proxy with a synthetic DS3-like frame:
//   cbSceneParam upload (960 bytes) -> ClearDepthStencilView on a D32S8 depth
//   -> Draw with the game's first post-process pixel shader (393A947C35535471)
//   reading an RGBA16F scene -> Present.
// usage: dlaa_test <proxy d3d11.dll> <393A947C35535471.ps.cso> [mv|edge|object|effect]
// With "mv" ([DLSS] DebugView=1 in the ini) the back buffer shows
// |motion vector| * 0.25, and the test checks it against the analytic value.
// With "edge" a slanted white edge is rendered through the (jittered) camera
// constants for 48 static frames, and the result is compared with a
// supersampled reference: the error shows whether [DLSS] JitterSign is right.
// With "object" ([DLSS] DebugView=1) the camera stays still while a quad moves
// right; it is also drawn by a velocity vertex shader like the game's motion
// blur pass, and the test checks the object motion vectors DLSS receives.
// With "effect" ([DLSS] DebugView=2) an additive quad (a transparent effect) and
// an opaque quad are drawn into the scene plus a second target, like DS3's
// forward pass; the reactive mask must cover only the additive one.
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

// Same sequence as camera/scene_camera.cpp.
double Halton(uint32_t index, uint32_t base) {
  double f = 1.0, r = 0.0;
  for (; index; index /= base) {
    f /= base;
    r += f * (index % base);
  }
  return r;
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
  const bool object = argc > 3 && wcscmp(argv[3], L"object") == 0;
  const bool effect = argc > 3 && wcscmp(argv[3], L"effect") == 0;
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
  desc.BufferCount = 1;
  desc.OutputWindow = hwnd;
  desc.Windowed = TRUE;
  // Sequential keeps the back buffer after Present, where DebugView is drawn.
  desc.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL;
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
  // Mip bias: the proxy binds a biased twin of samplers that use mips, during
  // the scene only. Shadow (comparison) samplers and the point sampler stay.
  ID3D11SamplerState* aniso_sampler = nullptr;
  ID3D11SamplerState* shadow_sampler = nullptr;
  {
    D3D11_SAMPLER_DESC ad = sd;
    ad.Filter = D3D11_FILTER_ANISOTROPIC;
    ad.MaxAnisotropy = 8;
    device->CreateSamplerState(&ad, &aniso_sampler);
    ad.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    ad.ComparisonFunc = D3D11_COMPARISON_LESS;
    device->CreateSamplerState(&ad, &shadow_sampler);
  }
  // Binds the three samplers and returns the MipLODBias of what got bound.
  auto bound_biases = [&](float* out) {
    ID3D11SamplerState* set[3] = {aniso_sampler, shadow_sampler, sampler};
    ctx->PSSetSamplers(4, 3, set);
    ID3D11SamplerState* got[3] = {};
    ctx->PSGetSamplers(4, 3, got);
    for (int i = 0; i < 3; ++i) {
      D3D11_SAMPLER_DESC d = {};
      if (got[i]) got[i]->GetDesc(&d);
      out[i] = got[i] ? d.MipLODBias : 99.0f;
      if (got[i]) got[i]->Release();
    }
    ID3D11SamplerState* none[3] = {};
    ctx->PSSetSamplers(4, 3, none);
  };
  int mip_bias_failures = 0;

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
  ID3D11Texture2D* second = nullptr;  // DS3's forward pass also writes a second target
  device->CreateTexture2D(&td, nullptr, &second);
  ID3D11RenderTargetView* forward_rtvs[2] = {scene_rtv, nullptr};
  device->CreateRenderTargetView(second, nullptr, &forward_rtvs[1]);
  td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;  // like DS3's T0 (bind 0x48)
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
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
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  ID3D11Texture2D* back_staging = nullptr;
  device->CreateTexture2D(&td, nullptr, &back_staging);
  ID3D11Texture2D* back_buffer = nullptr;
  swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
  // DebugView pixels (RGBA8 back buffer after Present), as floats.
  struct Pixel {
    float r, g, b;
  };
  auto read_back = [&](UINT x, UINT y) {
    ctx->CopyResource(back_staging, back_buffer);
    D3D11_MAPPED_SUBRESOURCE m = {};
    Pixel p = {-1, -1, -1};
    if (FAILED(ctx->Map(back_staging, 0, D3D11_MAP_READ, 0, &m))) return p;
    const auto* px = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4;
    p = {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
    ctx->Unmap(back_staging, 0);
    return p;
  };
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

  // Object scene: a quad at view depth kObjZ, moving right by kObjStep per frame
  // from frame 6. The scene pass draws it through the jittered b8 ViewProj; the
  // velocity pass draws it like DS3's motion blur shaders (cbMatrixWVP with
  // current/previous matrices, cbPrevObjMatrix), into a half-res RGBA8 target.
  constexpr double kObjZ = 4.0, kObjStep = 0.05, kObjStart = -0.3;
  const char* object_src =
      "cbuffer cbSceneParam : register(b8) { float4 pad[24]; row_major float4x4 vp; };"
      "cbuffer cbMatrixWVP : register(b0) { row_major float4x4 VC_MatrixWVP;"
      "  row_major float4x4 VC_PrevMatrixWVP; };"
      "cbuffer cbPrevObjMatrix : register(b6) { float4 VC_Offsets; };"  // x: current, y: previous
      "static const float2 kQuad[6] = { float2(-0.5, -0.5), float2(-0.5, 0.5), float2(0.5, 0.5),"
      "  float2(-0.5, -0.5), float2(0.5, 0.5), float2(0.5, -0.5) };"
      "float3 Corner(uint id, float offset) { return float3(kQuad[id].x + offset, kQuad[id].y, 4.0); }"
      "float4 scene_vs(uint id : SV_VertexID) : SV_Position {"
      "  return mul(vp, float4(Corner(id, VC_Offsets.x), 1)); }"
      "float4 scene_ps() : SV_Target { return float4(1, 0.2, 0.2, 1); }"
      "void velocity_vs(uint id : SV_VertexID, out float4 pos : SV_Position,"
      "  out float4 cur : TEXCOORD0, out float4 prev : TEXCOORD1) {"
      "  cur = mul(VC_MatrixWVP, float4(Corner(id, VC_Offsets.x), 1));"
      "  prev = mul(VC_PrevMatrixWVP, float4(Corner(id, VC_Offsets.y), 1));"
      "  pos = cur; }"
      "float4 velocity_ps(float4 pos : SV_Position, float4 cur : TEXCOORD0,"
      "  float4 prev : TEXCOORD1) : SV_Target { return 0.5; }";
  auto compile = [&](const char* entry, const char* target) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    D3DCompile(object_src, std::strlen(object_src), nullptr, nullptr, nullptr, entry, target, 0,
               0, &blob, &errors);
    if (errors) std::printf("%s: %s\n", entry, static_cast<const char*>(errors->GetBufferPointer()));
    return blob;
  };
  ID3DBlob* obj_blobs[4] = {compile("scene_vs", "vs_5_0"), compile("scene_ps", "ps_5_0"),
                            compile("velocity_vs", "vs_5_0"), compile("velocity_ps", "ps_5_0")};
  for (ID3DBlob* b : obj_blobs)
    if (!b) return 1;
  ID3D11VertexShader* obj_scene_vs = nullptr;
  ID3D11PixelShader* obj_scene_ps = nullptr;
  ID3D11VertexShader* velocity_vs = nullptr;
  ID3D11PixelShader* velocity_ps = nullptr;
  device->CreateVertexShader(obj_blobs[0]->GetBufferPointer(), obj_blobs[0]->GetBufferSize(),
                             nullptr, &obj_scene_vs);
  device->CreatePixelShader(obj_blobs[1]->GetBufferPointer(), obj_blobs[1]->GetBufferSize(),
                            nullptr, &obj_scene_ps);
  // DS3 creates each velocity shader many times; only the last object is used here.
  std::vector<ID3D11VertexShader*> velocity_copies(100, nullptr);
  for (ID3D11VertexShader*& vs : velocity_copies)
    device->CreateVertexShader(obj_blobs[2]->GetBufferPointer(), obj_blobs[2]->GetBufferSize(),
                               nullptr, &vs);
  velocity_vs = velocity_copies.back();
  device->CreatePixelShader(obj_blobs[3]->GetBufferPointer(), obj_blobs[3]->GetBufferSize(),
                            nullptr, &velocity_ps);
  // cbMatrixWVP is 192 bytes in both of DS3's layouts: object (WVP, PrevWVP,
  // NowToPrevView; never jittered) and per-view (camera, ViewProj; jittered).
  D3D11_BUFFER_DESC wvp_desc = {192, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  ID3D11Buffer* wvp_cb = nullptr;
  device->CreateBuffer(&wvp_desc, nullptr, &wvp_cb);
  ID3D11Buffer* view_cb = nullptr;  // written with UpdateSubresource
  device->CreateBuffer(&wvp_desc, nullptr, &view_cb);
  D3D11_BUFFER_DESC dynamic_desc = {192, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                                    D3D11_CPU_ACCESS_WRITE, 0, 0};
  ID3D11Buffer* view_dynamic_cb = nullptr;  // written with Map/Unmap
  device->CreateBuffer(&dynamic_desc, nullptr, &view_dynamic_cb);
  D3D11_BUFFER_DESC cb_staging_desc = {960, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
  ID3D11Buffer* cb_staging = nullptr;
  device->CreateBuffer(&cb_staging_desc, nullptr, &cb_staging);
  // Reads a constant buffer back as the GPU sees it.
  auto read_cb = [&](ID3D11Buffer* buffer, UINT bytes, float* out) {
    const D3D11_BOX box = {0, 0, 0, bytes, 1, 1};
    ctx->CopySubresourceRegion(cb_staging, 0, 0, 0, 0, buffer, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(cb_staging, 0, D3D11_MAP_READ, 0, &m))) return false;
    std::memcpy(out, m.pData, bytes);
    ctx->Unmap(cb_staging, 0);
    return true;
  };
  D3D11_BUFFER_DESC offsets_desc = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  ID3D11Buffer* offsets_cb = nullptr;
  device->CreateBuffer(&offsets_desc, nullptr, &offsets_cb);
  D3D11_TEXTURE2D_DESC vd = {};
  vd.Width = kW / 2;
  vd.Height = kH / 2;
  vd.MipLevels = 1;
  vd.ArraySize = 1;
  vd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  vd.SampleDesc.Count = 1;
  vd.Usage = D3D11_USAGE_DEFAULT;
  vd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* velocity_tex = nullptr;
  device->CreateTexture2D(&vd, nullptr, &velocity_tex);
  ID3D11RenderTargetView* velocity_rtv = nullptr;
  device->CreateRenderTargetView(velocity_tex, nullptr, &velocity_rtv);
  const double obj_expected = P0() * kObjStep / kObjZ * kW / 2 * 0.25;

  // Effect scene: quads in NDC. Additive (0.5 over the 0.5 grey scene: half the
  // final brightness) in the middle, opaque on the left.
  const char* effect_src =
      "cbuffer Quad : register(b0) { float4 rect; float4 colour; };"
      "float4 vs_main(uint id : SV_VertexID) : SV_Position {"
      "  const float2 c[6] = { float2(0, 0), float2(0, 1), float2(1, 1), float2(0, 0),"
      "    float2(1, 1), float2(1, 0) };"
      "  return float4(lerp(rect.xy, rect.zw, c[id]), 0.5, 1); }"
      "void ps_main(out float4 o0 : SV_Target0, out float4 o1 : SV_Target1) {"
      "  o0 = colour; o1 = 1; }";
  auto compile_effect = [&](const char* entry, const char* target) {
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    D3DCompile(effect_src, std::strlen(effect_src), nullptr, nullptr, nullptr, entry, target, 0,
               0, &blob, &errors);
    if (errors) std::printf("%s: %s\n", entry, static_cast<const char*>(errors->GetBufferPointer()));
    return blob;
  };
  ID3DBlob* effect_blobs[2] = {compile_effect("vs_main", "vs_5_0"),
                               compile_effect("ps_main", "ps_5_0")};
  if (!effect_blobs[0] || !effect_blobs[1]) return 1;
  ID3D11VertexShader* effect_vs = nullptr;
  ID3D11PixelShader* effect_ps = nullptr;
  device->CreateVertexShader(effect_blobs[0]->GetBufferPointer(),
                             effect_blobs[0]->GetBufferSize(), nullptr, &effect_vs);
  device->CreatePixelShader(effect_blobs[1]->GetBufferPointer(), effect_blobs[1]->GetBufferSize(),
                            nullptr, &effect_ps);
  D3D11_BUFFER_DESC quad_desc = {32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  ID3D11Buffer* quad_cb = nullptr;
  device->CreateBuffer(&quad_desc, nullptr, &quad_cb);
  D3D11_BLEND_DESC add_desc = {};
  add_desc.RenderTarget[0].BlendEnable = TRUE;
  add_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
  add_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  add_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  add_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  add_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  add_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  add_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  add_desc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  ID3D11BlendState* additive = nullptr;
  device->CreateBlendState(&add_desc, &additive);
  D3D11_DEPTH_STENCIL_DESC no_write_desc = {};
  no_write_desc.DepthEnable = FALSE;
  no_write_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ID3D11DepthStencilState* no_depth_write = nullptr;
  device->CreateDepthStencilState(&no_write_desc, &no_depth_write);

  D3D11_BUFFER_DESC cb_desc = {960, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  ID3D11Buffer* scene_cb = nullptr;
  device->CreateBuffer(&cb_desc, nullptr, &scene_cb);
  // Some DS3 areas upload a half-resolution copy of cbSceneParam before the
  // main one. It must be jittered like the scene (same NDC offset) without
  // becoming the camera DLAA uses; otherwise DLAA skips every such frame.
  ID3D11Buffer* half_cb = nullptr;
  device->CreateBuffer(&cb_desc, nullptr, &half_cb);
  int half_failures = 0;

  // Expected |mv.x| in the moving phase for the cleared depth plane.
  const double z = kNear * kProj10 / (kProj10 - kDepth);
  const double expected = P0() * kStep / z * kW / 2 * 0.25;
  int failures = 0;
  int silhouette_failures = 0;  // object mode: frames whose silhouette was off
  double cx = 0.0;
  alignas(16) float cb[240];
  const int frames = edge ? 48 : 12;
  for (int frame = 0; frame < frames; ++frame) {
    if (frame >= 6 && !edge && !object && !effect) cx += kStep;
    FillSceneCb(cb, cx);
    {
      alignas(16) float half[240];
      std::memcpy(half, cb, sizeof(half));
      half[4] = kW / 2;
      half[5] = kH / 2;
      half[6] = 2.0f / kW;
      half[7] = 2.0f / kH;
      ctx->UpdateSubresource(half_cb, 0, nullptr, half, 0, 0);
    }
    ctx->UpdateSubresource(scene_cb, 0, nullptr, cb, 0, 0);
    if (frame >= 2) {
      float gpu_full[240], gpu_half[240];
      if (read_cb(scene_cb, 960, gpu_full) && read_cb(half_cb, 960, gpu_half)) {
        // Identical matrices (same NDC jitter), and jittered at all.
        const bool same = std::memcmp(gpu_full + 32, gpu_half + 32, 128 * sizeof(float)) == 0;
        const bool jittered = gpu_full[96 + 2] != cb[96 + 2] || gpu_full[96 + 6] != cb[96 + 6];
        if (!same || !jittered || frame == frames - 1)
          std::printf("frame %d: half-res camera copy jittered like the scene %d, scene jittered "
                      "%d %s\n", frame, same, jittered, same && jittered ? "ok" : "MISMATCH");
        if (!same || !jittered) ++half_failures;
      }
    }
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, kDepth, 0);
    const float grey[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    ctx->ClearRenderTargetView(scene_rtv, edge ? black : grey);
    const D3D11_VIEWPORT viewport = {0, 0, kW, kH, 0, 1};
    if (object) {
      const float offsets[4] = {
          static_cast<float>(kObjStart + kObjStep * (frame >= 6 ? frame - 5 : 0)),
          static_cast<float>(kObjStart + kObjStep * (frame >= 7 ? frame - 6 : 0)), 0, 0};
      ctx->UpdateSubresource(offsets_cb, 0, nullptr, offsets, 0, 0);
      float wvp[48] = {};  // unjittered camera ViewProj (b8 offset 384), current and previous
      std::memcpy(wvp, cb + 96, 64);
      std::memcpy(wvp + 16, cb + 96, 64);
      ctx->UpdateSubresource(wvp_cb, 0, nullptr, wvp, 0, 0);
      // Per-view cbMatrixWVP: camera matrix (row 3 = position), then the
      // camera-relative ViewProj. It must get the same jitter as cbSceneParam.
      float view[48] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, cb[12], cb[13], cb[14], 1};
      std::memcpy(view + 16, cb + 96, 64);
      ctx->UpdateSubresource(view_cb, 0, nullptr, view, 0, 0);
      D3D11_MAPPED_SUBRESOURCE mapped = {};
      if (SUCCEEDED(ctx->Map(view_dynamic_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, view, sizeof(view));
        ctx->Unmap(view_dynamic_cb, 0);
      }
      float gpu_scene[240], gpu_view[48], gpu_dynamic[48], gpu_wvp[48];
      if (frame >= 2 && read_cb(scene_cb, 960, gpu_scene) && read_cb(view_cb, 192, gpu_view) &&
          read_cb(view_dynamic_cb, 192, gpu_dynamic) && read_cb(wvp_cb, 192, gpu_wvp)) {
        const bool jittered = std::memcmp(gpu_scene + 96, cb + 96, 64) != 0;
        const bool same = std::memcmp(gpu_view + 16, gpu_scene + 96, 64) == 0 &&
                          std::memcmp(gpu_dynamic + 16, gpu_scene + 96, 64) == 0 &&
                          std::memcmp(gpu_view, view, 64) == 0;
        const bool untouched = std::memcmp(gpu_wvp, wvp, sizeof(wvp)) == 0;
        if (!jittered || !same || !untouched) {
          std::printf("frame %d: view cb jitter: scene jittered %d, view cbs match it %d, "
                      "object cb untouched %d MISMATCH\n",
                      frame, jittered, same, untouched);
          ++failures;
        } else if (frame == frames - 1) {
          std::printf("view cb: jittered like cbSceneParam (UpdateSubresource and Map), "
                      "object cb untouched ok\n");
        }
      }
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->RSSetState(no_cull);
      // Scene pass (jittered, writes depth).
      ctx->OMSetRenderTargets(1, &scene_rtv, dsv);
      ctx->RSSetViewports(1, &viewport);
      ctx->VSSetShader(obj_scene_vs, nullptr, 0);
      ctx->VSSetConstantBuffers(8, 1, &scene_cb);
      ctx->VSSetConstantBuffers(6, 1, &offsets_cb);
      ctx->PSSetShader(obj_scene_ps, nullptr, 0);
      ctx->Draw(6, 0);
      // Velocity pass, half resolution, like DS3's motion blur.
      const float mid[4] = {0.5f, 0.5f, 0.5f, 0.0f};
      ctx->ClearRenderTargetView(velocity_rtv, mid);
      ctx->OMSetRenderTargets(1, &velocity_rtv, nullptr);
      const D3D11_VIEWPORT half = {0, 0, kW / 2, kH / 2, 0, 1};
      ctx->RSSetViewports(1, &half);
      ctx->VSSetShader(velocity_vs, nullptr, 0);
      ctx->VSSetConstantBuffers(0, 1, &wvp_cb);
      ctx->PSSetShader(velocity_ps, nullptr, 0);
      ctx->Draw(6, 0);
      ctx->RSSetState(nullptr);
    }
    if (effect) {
      ctx->OMSetRenderTargets(2, forward_rtvs, dsv);
      ctx->RSSetViewports(1, &viewport);
      ctx->RSSetState(no_cull);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetShader(effect_vs, nullptr, 0);
      ctx->VSSetConstantBuffers(0, 1, &quad_cb);
      ctx->PSSetShader(effect_ps, nullptr, 0);
      ctx->PSSetConstantBuffers(0, 1, &quad_cb);
      const float opaque_quad[8] = {-0.9f, -0.3f, -0.5f, 0.3f, 1, 1, 1, 1};
      ctx->UpdateSubresource(quad_cb, 0, nullptr, opaque_quad, 0, 0);
      ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
      ctx->Draw(6, 0);  // default depth state: writes depth
      ctx->OMSetDepthStencilState(no_depth_write, 0);
      const float additive_quad[8] = {-0.25f, -0.25f, 0.25f, 0.25f, 0.5f, 0.5f, 0.5f, 0.5f};
      ctx->UpdateSubresource(quad_cb, 0, nullptr, additive_quad, 0, 0);
      ctx->OMSetBlendState(additive, nullptr, 0xFFFFFFFF);
      ctx->Draw(6, 0);
      ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
      ctx->OMSetDepthStencilState(nullptr, 0);
      ctx->RSSetState(nullptr);
    }
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
    float scene_bias[3] = {};
    bound_biases(scene_bias);
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
    float post_bias[3] = {};
    bound_biases(post_bias);
    if (frame >= 2) {
      // DLAA starts running at the first Present; after it ran, no bias.
      const bool ok = scene_bias[0] < -0.01f && scene_bias[1] == 0.0f && scene_bias[2] == 0.0f &&
                      post_bias[0] == 0.0f && post_bias[1] == 0.0f && post_bias[2] == 0.0f;
      if (!ok || frame == 2)
        std::printf("frame %2d: mip bias scene (aniso %.2f, shadow %.2f, point %.2f) "
                    "post (%.2f, %.2f, %.2f) %s\n",
                    frame, scene_bias[0], scene_bias[1], scene_bias[2], post_bias[0], post_bias[1],
                    post_bias[2], ok ? "ok" : "MISMATCH");
      if (!ok) ++mip_bias_failures;
    }
    ID3D11ShaderResourceView* no_srv = nullptr;  // the scene is a render target again next frame
    ctx->PSSetShaderResources(0, 1, &no_srv);
    ctx->CSSetShaderResources(3, 1, &no_srv);
    swap_chain->Present(0, 0);

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
    if (effect && frame == 11) {
      const Pixel in = read_back(kW / 2, kH / 2);                // additive quad
      const Pixel opaque = read_back(kW * 15 / 100, kH / 2);     // opaque quad
      const Pixel out = read_back(kW / 2, kH / 10);              // untouched
      const bool ok = std::fabs(in.r - 0.5f) < 0.02f && opaque.r < 0.01f && out.r < 0.01f;
      std::printf("effect: reactive mask %.3f on the additive quad (expected 0.5), %.3f on the "
                  "opaque quad, %.3f elsewhere (expected 0) %s\n",
                  in.r, opaque.r, out.r, ok ? "ok" : "MISMATCH");
      if (!ok) ++failures;
    }
    if (object && (frame == 5 || frame == 11)) {
      const Pixel in = read_back(kW / 2, kH / 2);   // on the object
      const Pixel out = read_back(kW / 8, kH / 2);  // background
      const float r = in.r, g = in.g, b = in.b;
      const float br = out.r, bb = out.b;
      const double want = frame == 5 ? 0.0 : obj_expected;
      const bool ok = std::fabs(r - want) < 0.01 && std::fabs(g) < 0.01 && b > 0.9f &&
                      std::fabs(br) < 0.01 && bb < 0.5f;
      std::printf("frame %2d: object |mv|*0.25 = (%.4f, %.4f) flag %.2f, expected (%.4f, 0) flag 1;"
                  " background %.4f flag %.2f %s\n",
                  frame, r, g, b, want, br, bb, ok ? "ok" : "MISMATCH");
      if (!ok) ++failures;
    }
    if (object && frame >= 6) {
      // Silhouette: the object flag must cover exactly the pixels the jittered
      // scene pass covered (pixel centres inside the quad shifted by the jitter).
      const uint32_t n = static_cast<uint32_t>(frame) % 32 + 1;  // the proxy's jitter index
      const double ox = Halton(n, 2) - 0.5;
      const double offset = kObjStart + kObjStep * (frame - 5);
      const double left = (P0() * (-0.5 + offset) / kObjZ + 1) / 2 * kW + ox;
      const double right = (P0() * (0.5 + offset) / kObjZ + 1) / 2 * kW + ox;
      int wrong = 0, checked = 0;
      for (UINT x = static_cast<UINT>(left) - 3; x <= static_cast<UINT>(right) + 3; ++x) {
        const double centre = x + 0.5;
        if (std::fabs(centre - left) < 0.02 || std::fabs(centre - right) < 0.02) continue;
        const bool inside = centre >= left && centre < right;
        const bool flagged = read_back(x, kH / 2).b > 0.9f;
        wrong += inside != flagged;
        ++checked;
      }
      silhouette_failures += wrong != 0;
      if (wrong != 0) {
        std::printf("frame %2d: object silhouette: %d of %d pixels near the edges disagree with the "
                    "jittered scene (jitter x %+.3f) MISMATCH\n",
                    frame, wrong, checked, ox);
        ++failures;
      } else if (frame == frames - 1 && silhouette_failures == 0) {
        std::printf("object silhouette: matches the jittered scene on frames 6-%d ok\n", frame);
      }
    }
    if (!edge && !object && !effect && (frame == 5 || frame == 11)) {
      float r, g;
      if (check_mv) {  // DebugView image on the back buffer
        const Pixel px = read_back(kW / 2, kH / 2);
        r = px.r;
        g = px.g;
      } else {
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
        r = HalfToFloat(px[0]);
        g = HalfToFloat(px[1]);
        ctx->Unmap(staging, 0);
      }
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
    print_messages(frame);
  }
  std::printf("mip bias: %s\n", mip_bias_failures ? "MISMATCH" : "ok");
  failures += half_failures;
  failures += mip_bias_failures;
  std::printf("removed reason 0x%08lX\n", device->GetDeviceRemovedReason());
  if (FAILED(device->GetDeviceRemovedReason())) ++failures;
  std::printf(failures ? "FAILED\n" : "OK\n");
  // Released by process exit; the proxy stays loaded like in the game.
  return failures ? 1 : 0;
}
