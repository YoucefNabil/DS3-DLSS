#include "dlss/capture.h"

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "core.h"

using Microsoft::WRL::ComPtr;

namespace ds3::capture {
namespace {

constexpr int kRing = 4;        // staging sets: a frame is read back kRing - 1 frames later
constexpr size_t kMaxQueued = 6;  // frames waiting for the disk before the game waits too
const char* const kNames[kSlotCount] = {"color", "depth", "objmv", "effects",
                                        "mv",    "bias",  "object", "output"};

struct Image {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT w = 0, h = 0, bpp = 0;
  std::vector<uint8_t> data;  // tightly packed rows
};

// One file set for the writer thread; `summary` jobs write capture.txt only.
struct Job {
  std::wstring dir;
  int index = 0;
  std::string meta;
  Image images[kSlotCount];
  bool summary = false;
};

struct Set {
  ComPtr<ID3D11Texture2D> tex[kSlotCount];
  bool used[kSlotCount] = {};
  std::string meta;
  int index = 0;
  bool pending = false;  // copied on the GPU, not read back yet
};

std::atomic<bool> g_requested{false};
// Render thread only.
bool g_active = false;
bool g_auto_done = false;  // CaptureAt fires once
int g_total = 0, g_taken = 0;
std::wstring g_dir;
Set g_ring[kRing];
int g_next = 0;
DWORD g_start_tick = 0;

// Writer thread.
std::mutex g_mutex;
std::condition_variable g_cv;
std::deque<std::unique_ptr<Job>> g_queue;
bool g_writer_started = false;

UINT BytesPerPixel(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
      return 8;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
      return 4;
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    default:
      return 0;
  }
}

// DDS with the DX10 header, which every DDS viewer and DirectXTex reads.
bool WriteDds(const std::wstring& path, const Image& img) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
  uint32_t h[32 + 5] = {};
  h[0] = 0x20534444;  // "DDS "
  h[1] = 124;
  h[2] = 0x100F;  // caps, height, width, pitch, pixel format
  h[3] = img.h;
  h[4] = img.w;
  h[5] = img.w * img.bpp;
  h[19] = 32;          // pixel format size
  h[20] = 0x4;         // fourCC
  h[21] = 0x30315844;  // "DX10"
  h[27] = 0x1000;      // texture
  h[32] = img.format;
  h[33] = 3;  // texture 2D
  h[35] = 1;  // array size
  bool ok = std::fwrite(h, sizeof(h), 1, f) == 1 &&
            std::fwrite(img.data.data(), 1, img.data.size(), f) == img.data.size();
  ok = std::fclose(f) == 0 && ok;
  return ok;
}

bool WriteText(const std::wstring& path, const std::string& text) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return std::fclose(f) == 0 && ok;
}

void WriterLoop() {
  for (;;) {
    std::unique_ptr<Job> job;
    {
      std::unique_lock lock(g_mutex);
      g_cv.wait(lock, [] { return !g_queue.empty(); });
      job = std::move(g_queue.front());
      g_queue.pop_front();
    }
    g_cv.notify_all();  // room in the queue again
    bool ok = true;
    if (job->summary) {
      ok = WriteText(job->dir + L"capture.txt", job->meta);
      Log("capture: %s written to %ls", ok ? "all frames" : "FAILED,", job->dir.c_str());
      continue;
    }
    wchar_t name[64];
    std::swprintf(name, 64, L"%03d.txt", job->index);
    ok = WriteText(job->dir + name, job->meta);
    for (int s = 0; s < kSlotCount; ++s) {
      if (job->images[s].data.empty()) continue;
      std::swprintf(name, 64, L"%03d_%hs.dds", job->index, kNames[s]);
      ok = WriteDds(job->dir + name, job->images[s]) && ok;
    }
    if (!ok) Log("capture: writing frame %d failed (disk full?)", job->index);
  }
}

void Push(std::unique_ptr<Job> job) {
  std::unique_lock lock(g_mutex);
  if (!g_writer_started) {
    g_writer_started = true;
    std::thread(WriterLoop).detach();
  }
  g_cv.wait(lock, [] { return g_queue.size() < kMaxQueued; });
  g_queue.push_back(std::move(job));
  lock.unlock();
  g_cv.notify_all();
}

// Waits for the GPU copies of `set`, hands them to the writer.
void Drain(ID3D11DeviceContext* ctx, Set& set) {
  if (!set.pending) return;
  set.pending = false;
  auto job = std::make_unique<Job>();
  job->dir = g_dir;
  job->index = set.index;
  job->meta = std::move(set.meta);
  for (int s = 0; s < kSlotCount; ++s) {
    if (!set.used[s]) continue;
    D3D11_TEXTURE2D_DESC d;
    set.tex[s]->GetDesc(&d);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (FAILED(ctx->Map(set.tex[s].Get(), 0, D3D11_MAP_READ, 0, &m))) continue;
    Image& img = job->images[s];
    img.w = d.Width;
    img.h = d.Height;
    // Depth keeps only its float (the stencil byte is not needed); typeless
    // colour is the game's RGBA16F.
    img.format = d.Format == DXGI_FORMAT_R32G8X24_TYPELESS       ? DXGI_FORMAT_R32_FLOAT
                 : d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                                 : d.Format;
    img.bpp = BytesPerPixel(img.format);
    const UINT src_bpp = BytesPerPixel(d.Format);
    img.data.resize(static_cast<size_t>(img.w) * img.h * img.bpp);
    for (UINT y = 0; y < img.h; ++y) {
      const auto* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
      uint8_t* dst = img.data.data() + static_cast<size_t>(y) * img.w * img.bpp;
      if (src_bpp == img.bpp) {
        std::memcpy(dst, src, static_cast<size_t>(img.w) * img.bpp);
      } else {
        for (UINT x = 0; x < img.w; ++x) std::memcpy(dst + x * 4, src + x * src_bpp, 4);
      }
    }
    ctx->Unmap(set.tex[s].Get(), 0);
  }
  Push(std::move(job));
}

void Start() {
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  std::swprintf(name, 64, L"DS3_DLSS_capture_%04u%02u%02u_%02u%02u%02u\\", t.wYear, t.wMonth,
                t.wDay, t.wHour, t.wMinute, t.wSecond);
  g_dir = ModuleDir() + name;
  if (!CreateDirectoryW(g_dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    Log("capture: cannot create %ls", g_dir.c_str());
    return;
  }
  CopyFileW((ModuleDir() + L"DS3_DLSS.ini").c_str(), (g_dir + L"settings.ini").c_str(), FALSE);
  g_total = GetConfig().capture_frames;
  g_taken = 0;
  g_next = 0;
  for (Set& s : g_ring) s.pending = false;
  g_active = true;
  g_start_tick = GetTickCount();
  Log("capture: %d frames into %ls", g_total, g_dir.c_str());
}

}  // namespace

void Request() { g_requested = true; }

bool Active() { return g_active; }

void BeginFrame(uint32_t frame) {
  if (!g_active) {
    const int at = GetConfig().capture_at;
    const bool auto_start = at > 0 && !g_auto_done && frame >= static_cast<uint32_t>(at);
    if (auto_start) g_auto_done = true;
    if (!g_requested.exchange(false) && !auto_start) return;
    if (GetConfig().capture_frames <= 0) return;
    Start();
    if (!g_active) return;
  }
  for (bool& u : g_ring[g_next].used) u = false;
}

void Grab(ID3D11DeviceContext* ctx, Slot slot, ID3D11Resource* source) {
  if (!g_active || !source) return;
  ComPtr<ID3D11Texture2D> src;
  if (FAILED(source->QueryInterface(IID_PPV_ARGS(&src)))) return;
  D3D11_TEXTURE2D_DESC d;
  src->GetDesc(&d);
  if (!BytesPerPixel(d.Format) || d.MipLevels != 1 || d.ArraySize != 1 || d.SampleDesc.Count != 1)
    return;
  Set& set = g_ring[g_next];
  ComPtr<ID3D11Texture2D>& tex = set.tex[slot];
  D3D11_TEXTURE2D_DESC have = {};
  if (tex) tex->GetDesc(&have);
  if (!tex || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Device> device;
    ctx->GetDevice(&device);
    tex.Reset();
    if (FAILED(device->CreateTexture2D(&d, nullptr, &tex))) {
      Log("capture: staging texture for %s failed", kNames[slot]);
      return;
    }
  }
  ctx->CopyResource(tex.Get(), src.Get());
  set.used[slot] = true;
}

void Finish(ID3D11DeviceContext* ctx, const char* meta) {
  if (!g_active) return;
  Set& set = g_ring[g_next];
  set.meta = meta;
  set.index = g_taken++;
  set.pending = true;
  g_next = (g_next + 1) % kRing;
  if (g_taken < g_total) {
    Drain(ctx, g_ring[g_next]);  // the oldest, reused next frame
    return;
  }
  for (int i = 0; i < kRing; ++i) Drain(ctx, g_ring[(g_next + i) % kRing]);
  for (Set& s : g_ring) s = Set{};  // staging memory back
  g_active = false;
  const DWORD ms = GetTickCount() - g_start_tick;
  char text[256];
  std::snprintf(text, sizeof(text),
                "frames %d\r\ncaptured_in_ms %lu\r\nformat 1\r\n", g_taken,
                static_cast<unsigned long>(ms));
  auto job = std::make_unique<Job>();
  job->dir = g_dir;
  job->meta = text;
  job->summary = true;
  Push(std::move(job));
  Log("capture: %d frames taken in %lu ms, writing in the background", g_taken,
      static_cast<unsigned long>(ms));
}

}  // namespace ds3::capture
