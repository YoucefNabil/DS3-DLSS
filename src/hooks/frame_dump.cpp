#include "hooks/frame_dump.h"

#include <windows.h>
#include <d3d11_1.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log.h"
#include "hooks/shader_registry.h"

namespace ds3::frame_dump {

std::atomic<bool> g_active{false};

namespace {

struct Entry {
  std::string key;  // everything except the call count, used to collapse repeats
  std::string text;
  uint32_t repeats = 1;
  uint64_t total_count = 0;
};

// Idle -> (request) -> Pre: one frame recording deferred contexts only, so
// command lists built a frame ahead are known -> Main: the dumped frame,
// immediate + deferred contexts -> file written, back to Idle.
enum class Phase { Idle, Pre, Main };

std::atomic<bool> g_requested{false};
std::mutex g_mutex;
Phase g_phase = Phase::Idle;
std::vector<Entry> g_entries;  // immediate context, Main frame only
std::unordered_map<ID3D11DeviceContext*, std::vector<Entry>> g_ctx_entries;  // open recordings
std::unordered_map<ID3D11CommandList*, std::vector<Entry>> g_list_entries;   // finished lists
std::unordered_map<ID3D11CommandList*, int> g_list_ids;
// Capture frame (-1 = Pre, 0 = the dumped frame) in which each deferred
// recording started and finished, to see how far ahead lists are built.
int g_frame = -1;
std::unordered_map<ID3D11DeviceContext*, int> g_ctx_start;
std::unordered_map<ID3D11CommandList*, std::pair<int, int>> g_list_frames;
std::unordered_map<ID3D11Resource*, int> g_resource_ids;
std::vector<std::string> g_resource_descs;
uint32_t g_execute_command_lists = 0;
uint32_t g_unrecorded_lists = 0;
struct PendingMap {
  const uint8_t* data;
  UINT bytes;
  D3D11_MAP type;
};
std::map<std::pair<ID3D11DeviceContext*, ID3D11Resource*>, PendingMap> g_pending_maps;

// cbSceneParam is ~960 bytes; per-object constant buffers are much smaller.
constexpr UINT kMinSceneCbBytes = 768;

const char* FormatName(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "RGBA32F";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "RGBA16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "RGBA16F";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "RGBA16_UNORM";
    case DXGI_FORMAT_R32G32_FLOAT: return "RG32F";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32S8";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "RGB10A2_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "RGB10A2";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10F";
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "RGBA8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "RGBA8_SRGB";
    case DXGI_FORMAT_R16G16_TYPELESS: return "RG16_TYPELESS";
    case DXGI_FORMAT_R16G16_FLOAT: return "RG16F";
    case DXGI_FORMAT_R16G16_SNORM: return "RG16_SNORM";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT: return "D32F";
    case DXGI_FORMAT_R32_FLOAT: return "R32F";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24S8";
    case DXGI_FORMAT_R8G8_UNORM: return "RG8";
    case DXGI_FORMAT_R16_TYPELESS: return "R16_TYPELESS";
    case DXGI_FORMAT_R16_FLOAT: return "R16F";
    case DXGI_FORMAT_D16_UNORM: return "D16";
    case DXGI_FORMAT_R8_UNORM: return "R8";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "BGRA8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "BGRA8_SRGB";
    case DXGI_FORMAT_BC1_UNORM: return "BC1";
    case DXGI_FORMAT_BC3_UNORM: return "BC3";
    case DXGI_FORMAT_BC7_UNORM: return "BC7";
    default: return nullptr;
  }
}

std::string DescribeNew(ID3D11Resource* r) {
  char buf[160];
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
    D3D11_TEXTURE2D_DESC d;
    static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
    const char* name = FormatName(d.Format);
    char fmt[24];
    if (!name) std::snprintf(fmt, sizeof(fmt), "fmt%d", d.Format), name = fmt;
    std::snprintf(buf, sizeof(buf), "tex2d %ux%u %s mips=%u array=%u msaa=%u bind=0x%X", d.Width,
                  d.Height, name, d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.BindFlags);
  } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
    D3D11_BUFFER_DESC d;
    static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
    std::snprintf(buf, sizeof(buf), "buffer %u bytes bind=0x%X", d.ByteWidth, d.BindFlags);
  } else {
    std::snprintf(buf, sizeof(buf), "resource dim=%d", dim);
  }
  return buf;
}

// Short per-dump id ("T12") for a resource; full descriptions go in a table
// at the end of the file.
std::string Id(ID3D11Resource* r) {
  if (!r) return "-";
  auto [it, inserted] = g_resource_ids.try_emplace(r, static_cast<int>(g_resource_ids.size()));
  if (inserted) g_resource_descs.push_back(DescribeNew(r));
  return "T" + std::to_string(it->second);
}

std::string ViewId(ID3D11View* v) {
  if (!v) return "-";
  ID3D11Resource* r = nullptr;
  v->GetResource(&r);
  std::string id = Id(r);
  if (r) r->Release();
  return id;
}

template <typename View>
std::string ViewList(View* const* views, UINT n) {
  UINT last = 0;
  for (UINT i = 0; i < n; ++i)
    if (views[i]) last = i + 1;
  std::string s = "[";
  for (UINT i = 0; i < last; ++i) {
    if (i) s += ',';
    s += ViewId(views[i]);
  }
  return s + "]";
}

template <typename T>
void ReleaseAll(T* const* items, UINT n) {
  for (UINT i = 0; i < n; ++i)
    if (items[i]) items[i]->Release();
}

std::string Hex(uint64_t v) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
  return buf;
}

// Where a call on ctx is recorded, or nullptr if it isn't recorded right now
// (the immediate context is only recorded during the Main frame).
std::vector<Entry>* Target(ID3D11DeviceContext* ctx) {
  if (g_phase == Phase::Idle) return nullptr;
  if (ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
    g_ctx_start.try_emplace(ctx, g_frame);
    return &g_ctx_entries[ctx];
  }
  return g_phase == Phase::Main ? &g_entries : nullptr;
}

void Push(std::vector<Entry>& out, std::string key, const char* op, uint64_t count) {
  if (!out.empty() && out.back().key == key) {
    out.back().repeats++;
    out.back().total_count += count;
    return;
  }
  Entry e;
  e.text = std::string(op) + "  " + key;
  e.key = std::move(key);
  e.total_count = count;
  out.push_back(std::move(e));
}

// Constant buffer bound at slot 8 (cbSceneParam in DS3's shaders), with the
// D3D11.1 first-constant offset if a range of a larger buffer is bound.
std::string SceneCbId(ID3D11DeviceContext* ctx, bool pixel) {
  ID3D11Buffer* b = nullptr;
  UINT first = 0, num = 0;
  ID3D11DeviceContext1* ctx1 = nullptr;
  if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
    if (pixel) ctx1->PSGetConstantBuffers1(8, 1, &b, &first, &num);
    else ctx1->VSGetConstantBuffers1(8, 1, &b, &first, &num);
    ctx1->Release();
  } else if (pixel) {
    ctx->PSGetConstantBuffers(8, 1, &b);
  } else {
    ctx->VSGetConstantBuffers(8, 1, &b);
  }
  if (!b) return "-";
  std::string s = Id(b);
  if (first) s += "+" + std::to_string(first * 16);
  b->Release();
  return s;
}

// Byte size if r is a constant buffer big enough to be cbSceneParam, else 0.
UINT SceneSizedCb(ID3D11Resource* r) {
  if (!r) return 0;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return 0;
  D3D11_BUFFER_DESC d;
  static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
  if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || d.ByteWidth < kMinSceneCbBytes) return 0;
  return d.ByteWidth;
}

std::string Floats(const uint8_t* p, int count) {
  std::string s = "[";
  char buf[24];
  for (int i = 0; i < count; ++i) {
    float f;
    std::memcpy(&f, p + i * 4, 4);
    std::snprintf(buf, sizeof(buf), i ? " %.6g" : "%.6g", f);
    s += buf;
  }
  return s + "]";
}

// Decodes the interesting cbSceneParam members (offsets from the shaders'
// reflection data). For other large buffers this is just raw numbers.
std::string DescribeSceneCb(const uint8_t* p, UINT bytes) {
  std::string s;
  if (bytes >= 64) s += " ScreenSize=" + Floats(p + 16, 4) + " CameraPos=" + Floats(p + 48, 4);
  if (bytes >= 320) s += " Proj=" + Floats(p + 256, 16);
  if (bytes >= 448) s += " ViewProj=" + Floats(p + 384, 16);
  if (bytes >= 192) s += " InvViewProj=" + Floats(p + 128, 16);
  return s;
}

std::string ListId(ID3D11CommandList* list) {
  auto [it, inserted] = g_list_ids.try_emplace(list, static_cast<int>(g_list_ids.size()));
  return "L" + std::to_string(it->second);
}

void WriteFile() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf_s(name, L"DS3_DLSS_frame_%04u%02u%02u_%02u%02u%02u_%03u.txt", t.wYear, t.wMonth,
             t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
  const std::wstring path = ModuleDir() + name;
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"w") != 0 || !f) {
    Log("frame dump: could not write %ls", path.c_str());
    return;
  }
  std::fprintf(f, "# One frame of GPU work, in submission order. Deferred command lists are\n");
  std::fprintf(f, "# spliced in (lines starting with '|') where the immediate context executes them.\n");
  std::fprintf(f, "# Consecutive identical calls are collapsed as 'xN'. Resource ids are listed at the end.\n\n");
  for (size_t i = 0; i < g_entries.size(); ++i) {
    const Entry& e = g_entries[i];
    std::fprintf(f, "#%04zu x%-4u n=%-8llu %s\n", i, e.repeats,
                 static_cast<unsigned long long>(e.total_count), e.text.c_str());
  }
  std::fprintf(f, "\n# Resources\n");
  for (size_t i = 0; i < g_resource_descs.size(); ++i)
    std::fprintf(f, "T%zu  %s\n", i, g_resource_descs[i].c_str());
  std::fprintf(f, "\n# ExecuteCommandList calls: %u (%u with a list not recorded during capture)\n",
               g_execute_command_lists, g_unrecorded_lists);
  std::fclose(f);
  Log("frame dump written: %ls (%zu entries, %zu resources, %u lists, %u unrecorded)", path.c_str(),
      g_entries.size(), g_resource_descs.size(), g_execute_command_lists, g_unrecorded_lists);
}

}  // namespace

void Request() { g_requested = true; }

void OnPresent() {
  std::lock_guard lock(g_mutex);
  switch (g_phase) {
    case Phase::Main:
      WriteFile();
      g_phase = Phase::Idle;
      g_active = false;
      g_entries = {};
      g_ctx_entries = {};
      g_list_entries = {};
      g_list_ids = {};
      g_resource_ids = {};
      g_resource_descs = {};
      g_pending_maps = {};
      g_ctx_start = {};
      g_list_frames = {};
      return;
    case Phase::Pre:
      g_phase = Phase::Main;
      g_frame = 0;
      return;
    case Phase::Idle:
      if (!g_requested.exchange(false)) return;
      g_execute_command_lists = 0;
      g_unrecorded_lists = 0;
      g_phase = Phase::Pre;
      g_frame = -1;
      g_active = true;
      Log("frame dump: capturing (deferred lists this frame, full frame next)");
      return;
  }
}

void RecordDraw(ID3D11DeviceContext* ctx, const char* op, UINT count, UINT instances) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ctx->VSGetShader(&vs, nullptr, nullptr);
  ctx->PSGetShader(&ps, nullptr, nullptr);
  ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
  ID3D11ShaderResourceView* srvs[8] = {};
  ctx->PSGetShaderResources(0, 8, srvs);
  D3D11_VIEWPORT vp = {};
  UINT num_vp = 1;
  ctx->RSGetViewports(&num_vp, &vp);

  char vp_text[32];
  std::snprintf(vp_text, sizeof(vp_text), "%.0fx%.0f", vp.Width, vp.Height);
  std::string key = "VS=" + Hex(shaders::Lookup(vs)) + " PS=" + Hex(shaders::Lookup(ps)) +
                    " RT=" + ViewList(rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT) +
                    " DS=" + ViewId(dsv) + " VP=" + vp_text + " PS.SRV=" + ViewList(srvs, 8) +
                    " CB8=" + SceneCbId(ctx, false) + "/" + SceneCbId(ctx, true);
  if (instances > 1) key += " inst=" + std::to_string(instances);
  Push(*out, std::move(key), op, count);

  if (vs) vs->Release();
  if (ps) ps->Release();
  if (dsv) dsv->Release();
  ReleaseAll(rtvs, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT);
  ReleaseAll(srvs, 8);
}

void RecordDispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;

  ID3D11ComputeShader* cs = nullptr;
  ctx->CSGetShader(&cs, nullptr, nullptr);
  ID3D11UnorderedAccessView* uavs[4] = {};
  ctx->CSGetUnorderedAccessViews(0, 4, uavs);
  ID3D11ShaderResourceView* srvs[8] = {};
  ctx->CSGetShaderResources(0, 8, srvs);

  char groups[48];
  std::snprintf(groups, sizeof(groups), " groups=%ux%ux%u", x, y, z);
  std::string key = "CS=" + Hex(shaders::Lookup(cs)) + groups + " UAV=" + ViewList(uavs, 4) +
                    " CS.SRV=" + ViewList(srvs, 8);
  Push(*out, std::move(key), "Dispatch", 1);

  if (cs) cs->Release();
  ReleaseAll(uavs, 4);
  ReleaseAll(srvs, 8);
}

void RecordCopy(ID3D11DeviceContext* ctx, const char* op, ID3D11Resource* dst, ID3D11Resource* src) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  Push(*out, "dst=" + Id(dst) + " src=" + Id(src), op, 1);
}

void RecordClear(ID3D11DeviceContext* ctx, const char* op, ID3D11View* view) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  Push(*out, "view=" + ViewId(view), op, 1);
}

void RecordExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list) {
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  const bool immediate = out == &g_entries;
  if (immediate) ++g_execute_command_lists;
  const std::string id = ListId(list);
  const auto it = g_list_entries.find(list);
  if (it == g_list_entries.end()) {
    if (immediate) ++g_unrecorded_lists;
    Push(*out, id + " (not recorded during capture)", "ExecuteCommandList", 1);
    return;
  }
  // Header and spliced entries get unique keys so they never collapse with
  // neighbouring calls.
  std::string header = id + " (" + std::to_string(it->second.size()) + " entries";
  if (const auto f = g_list_frames.find(list); f != g_list_frames.end())
    header += ", recorded frame " + std::to_string(f->second.first) + ".." +
              std::to_string(f->second.second);
  header += ")";
  Push(*out, header + "#" + std::to_string(out->size()), "ExecuteCommandList", 1);
  out->back().text = "ExecuteCommandList  " + header;
  for (const Entry& e : it->second) {
    Entry copy = e;
    copy.key = "|" + e.key + "#" + std::to_string(out->size());
    copy.text = "| " + e.text;
    out->push_back(std::move(copy));
  }
}

void RecordFinishCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list) {
  std::lock_guard lock(g_mutex);
  if (g_phase == Phase::Idle) return;
  auto node = g_ctx_entries.extract(ctx);
  g_list_entries[list] = node.empty() ? std::vector<Entry>{} : std::move(node.mapped());
  const auto start = g_ctx_start.find(ctx);
  g_list_frames[list] = {start != g_ctx_start.end() ? start->second : g_frame, g_frame};
  if (start != g_ctx_start.end()) g_ctx_start.erase(start);
}

void RecordMap(ID3D11DeviceContext* ctx, ID3D11Resource* r, D3D11_MAP type, void* data) {
  const UINT bytes = SceneSizedCb(r);
  if (!bytes || !data) return;
  std::lock_guard lock(g_mutex);
  if (!Target(ctx)) return;
  g_pending_maps[{ctx, r}] = {static_cast<const uint8_t*>(data), bytes, type};
}

void RecordUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* r) {
  std::lock_guard lock(g_mutex);
  const auto it = g_pending_maps.find({ctx, r});
  if (it == g_pending_maps.end()) return;
  const PendingMap m = it->second;
  g_pending_maps.erase(it);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  // The game has finished writing; the contents are what the GPU will see.
  const std::string key = "dst=" + Id(r) + " bytes=" + std::to_string(m.bytes) +
                          " type=" + std::to_string(m.type) + DescribeSceneCb(m.data, m.bytes);
  Push(*out, key, "Map/Unmap CB", 1);
}

void RecordUpdate(ID3D11DeviceContext* ctx, const char* op, ID3D11Resource* r,
                  const D3D11_BOX* box, const void* data) {
  const UINT bytes = SceneSizedCb(r);
  if (!bytes || !data) return;
  std::lock_guard lock(g_mutex);
  std::vector<Entry>* out = Target(ctx);
  if (!out) return;
  const UINT offset = box ? box->left : 0;
  const UINT size = box ? box->right - box->left : bytes;
  std::string key = "dst=" + Id(r) + " offset=" + std::to_string(offset) +
                    " bytes=" + std::to_string(size);
  if (offset == 0) key += DescribeSceneCb(static_cast<const uint8_t*>(data), size);
  Push(*out, key, op, 1);
}

}  // namespace ds3::frame_dump
