// Replays a DS3_DLSS capture ([Debug] CaptureKey in game) through DLSS and
// measures it against the input and the captured output on the same real frames.
//
// usage: capture_replay <capture folder> [runs...] [options]
//   runs     any of: input captured DLSS (default: all of them)
//            input    = the jittered scene without AA (the aliasing baseline)
//            captured = what the game showed while capturing
//            DLSS     = the proxy run on the captured frames
//   --dll <path>          proxy d3d11.dll (default: the one next to this exe);
//                         nvngx_dlss.dll is taken from the same folder
//   --out <folder>        results (default: <capture>\replay)
//   --warmup <n>          frames before measuring (default: min(16, frames / 2))
//   --set Sec.Key=Value   DS3_DLSS.ini override for the replays, e.g.
//                         --set DLSS.MipBias=-1 (repeatable)
//   --crop x,y,w,h        also write zoomed crops of the last frames
//   --zoom <n>            crop magnification (default 4)
//   --crop-frames <n>     how many last frames get a crop (default 8)
//   --save-dds            keep every replayed output as NNN_output.dds
//
// Each run writes <out>\<run>\: metrics.txt, last.png (tone mapped), flicker.png
// (per-pixel temporal std dev on static pixels, black = stable, yellow =
// shimmering) and the crops. The replays also keep the proxy, its ini and its
// DS3_DLSS.log in <out>\<run>\proxy\. A table of all runs is printed at the end.
//
// Metrics, on tone-mapped luma (x / (1 + x) after a fixed exposure from frame 0):
//   flicker       mean |change| between consecutive frames, static pixels
//                 (motion below 0.05 px in both, not a character; the camera's
//                 motion is recomputed from depth and the frame records, so
//                 the proxy's own vectors do not decide what counts as static)
//   flicker_det   the same on detail pixels (the 10% of always-static pixels
//                 with the strongest edges: foliage, railings, chainmail)
//   std, std_det  per-pixel std dev over the measured frames; std_p99 its 99th percentile
//   error, err_det  mean |output - reference|, reference = average of all
//                 jittered input frames (a supersampled image when the camera
//                 holds still)
//   sharp         edge strength of the time-averaged output relative to the
//                 reference on detail pixels (1 = as sharp, < 1 = blurrier)
//   moving        mean |change| against the previous frame moved along the
//                 motion vectors, on moving world pixels
//   object        the same on character pixels
// The static metrics need the camera to hold still for most of the capture;
// "static" in the table says which share of pixels qualified.

#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "dlss/replay_api.h"

using Microsoft::WRL::ComPtr;

namespace {

const char* const kAllRuns[] = {"input", "captured", "DLSS"};

struct Options {
  std::wstring capture, dll, out;
  std::vector<std::string> runs;
  std::vector<std::wstring> sets;
  int warmup = -1;
  int crop[4] = {};
  bool has_crop = false;
  int zoom = 4;
  int crop_frames = 8;
  bool save_dds = false;
  std::string child;  // replay this backend in this process
};

std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const std::wstring& s) {
  std::string r;
  for (wchar_t c : s) r += c < 128 ? static_cast<char>(c) : '?';
  return r;
}

// ---- files -----------------------------------------------------------------

struct Image {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  UINT w = 0, h = 0, bpp = 0;
  std::vector<uint8_t> data;
};

bool ReadFile(const std::wstring& path, std::vector<uint8_t>* out) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out->resize(size > 0 ? size : 0);
  const bool ok = size > 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
  std::fclose(f);
  return ok;
}

UINT BytesPerPixel(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
      return 8;
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
      return 4;
    case DXGI_FORMAT_R8_UNORM:
      return 1;
    default:
      return 0;
  }
}

// The DDS files capture.cpp writes (DX10 header, packed rows).
bool ReadDds(const std::wstring& path, Image* img) {
  std::vector<uint8_t> file;
  if (!ReadFile(path, &file) || file.size() < 148) return false;
  uint32_t h[37];
  std::memcpy(h, file.data(), sizeof(h));
  if (h[0] != 0x20534444 || h[21] != 0x30315844) return false;
  img->format = static_cast<DXGI_FORMAT>(h[32]);
  img->h = h[3];
  img->w = h[4];
  img->bpp = BytesPerPixel(img->format);
  const size_t bytes = static_cast<size_t>(img->w) * img->h * img->bpp;
  if (!img->bpp || file.size() < 148 + bytes) return false;
  img->data.assign(file.begin() + 148, file.begin() + 148 + bytes);
  return true;
}

bool WriteDds(const std::wstring& path, const Image& img) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
  uint32_t h[37] = {};
  h[0] = 0x20534444;
  h[1] = 124;
  h[2] = 0x100F;
  h[3] = img.h;
  h[4] = img.w;
  h[5] = img.w * img.bpp;
  h[19] = 32;
  h[20] = 0x4;
  h[21] = 0x30315844;
  h[27] = 0x1000;
  h[32] = img.format;
  h[33] = 3;
  h[35] = 1;
  bool ok = std::fwrite(h, sizeof(h), 1, f) == 1 &&
            std::fwrite(img.data.data(), 1, img.data.size(), f) == img.data.size();
  return std::fclose(f) == 0 && ok;
}

bool WritePng(const std::wstring& path, UINT w, UINT h, const std::vector<uint8_t>& bgr) {
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) &&
         SUCCEEDED(factory->CreateStream(&stream)) &&
         SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
         SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
         SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h)) &&
         SUCCEEDED(frame->SetPixelFormat(&format)) &&
         IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
         SUCCEEDED(frame->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()),
                                      const_cast<BYTE*>(bgr.data()))) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// Per-frame record (NNN.txt): "key value value ..." lines.
struct Meta {
  std::map<std::string, std::vector<float>> values;
  std::map<std::string, std::string> text;
  float F(const char* key, size_t i = 0) const {
    auto it = values.find(key);
    return it != values.end() && i < it->second.size() ? it->second[i] : 0.0f;
  }
  const float* V(const char* key, size_t n) const {
    auto it = values.find(key);
    return it != values.end() && it->second.size() >= n ? it->second.data() : nullptr;
  }
};

bool ReadMeta(const std::wstring& path, Meta* meta) {
  std::vector<uint8_t> file;
  if (!ReadFile(path, &file)) return false;
  std::string all(file.begin(), file.end());
  size_t pos = 0;
  while (pos < all.size()) {
    size_t end = all.find('\n', pos);
    if (end == std::string::npos) end = all.size();
    std::string line = all.substr(pos, end - pos);
    pos = end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const size_t space = line.find(' ');
    if (space == std::string::npos) continue;
    const std::string key = line.substr(0, space);
    meta->text[key] = line.substr(space + 1);
    std::vector<float>& v = meta->values[key];
    const char* p = line.c_str() + space;
    for (;;) {
      char* next = nullptr;
      const float x = std::strtof(p, &next);
      if (next == p) break;
      v.push_back(x);
      p = next;
    }
  }
  return true;
}

struct Capture {
  std::wstring dir;  // with trailing backslash
  int frames = 0;
  UINT w = 0, h = 0;
  std::vector<Meta> meta;
  std::wstring File(int i, const char* name) const {
    wchar_t n[64];
    std::swprintf(n, 64, L"%03d_%hs.dds", i, name);
    return dir + n;
  }
  bool Has(int i, const char* name) const {
    return GetFileAttributesW(File(i, name).c_str()) != INVALID_FILE_ATTRIBUTES;
  }
};

bool OpenCapture(std::wstring dir, Capture* cap) {
  if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
  cap->dir = dir;
  for (int i = 0;; ++i) {
    wchar_t n[16];
    std::swprintf(n, 16, L"%03d.txt", i);
    Meta m;
    if (!ReadMeta(dir + n, &m) || !cap->Has(i, "color")) break;
    cap->meta.push_back(std::move(m));
  }
  cap->frames = static_cast<int>(cap->meta.size());
  if (!cap->frames) return false;
  cap->w = static_cast<UINT>(cap->meta[0].F("size", 0));
  cap->h = static_cast<UINT>(cap->meta[0].F("size", 1));
  return cap->w && cap->h;
}

// ---- pixels ----------------------------------------------------------------

float g_half[65536];

void InitHalf() {
  for (uint32_t h = 0; h < 65536; ++h) {
    const uint32_t s = (h & 0x8000u) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff;
    float f;
    if (e == 0) {
      f = std::ldexp(static_cast<float>(m), -24);
      if (s) f = -f;
    } else {
      const uint32_t bits = e == 31 ? s | 0x7f800000u | (m << 13) : s | ((e + 112) << 23) | (m << 13);
      std::memcpy(&f, &bits, 4);
    }
    g_half[h] = f;
  }
}

float Half(const uint8_t* p) {
  uint16_t h;
  std::memcpy(&h, p, 2);
  return g_half[h];
}

float Clean(float x) { return std::isfinite(x) && x > 0.0f ? x : 0.0f; }

// RGB of pixel i of an RGBA16F image.
void Rgb(const Image& img, size_t i, float* c) {
  const uint8_t* p = img.data.data() + i * 8;
  for (int k = 0; k < 3; ++k) c[k] = Clean(Half(p + 2 * k));
}

float Tm(float x) { return x / (1.0f + x); }

// Luminance, then tone mapped, so Tm^-1 of it is linear again (see Analyzer).
float Luma(const float* c, float exposure) {
  return Tm(exposure * (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]));
}

std::vector<float> LumaImage(const Image& img, float exposure) {
  std::vector<float> y(static_cast<size_t>(img.w) * img.h);
  float c[3];
  for (size_t i = 0; i < y.size(); ++i) {
    Rgb(img, i, c);
    y[i] = Luma(c, exposure);
  }
  return y;
}

uint8_t Srgb8(float x) {
  x = std::clamp(x, 0.0f, 1.0f);
  return static_cast<uint8_t>(std::lround(std::pow(x, 1.0f / 2.2f) * 255.0f));
}

std::vector<uint8_t> ToneMapped(const Image& img, float exposure) {
  std::vector<uint8_t> bgr(static_cast<size_t>(img.w) * img.h * 3);
  float c[3];
  for (size_t i = 0; i < static_cast<size_t>(img.w) * img.h; ++i) {
    Rgb(img, i, c);
    for (int k = 0; k < 3; ++k) bgr[i * 3 + 2 - k] = Srgb8(Tm(c[k] * exposure));
  }
  return bgr;
}

// Nearest-neighbour crop and zoom of a BGR image.
std::vector<uint8_t> Crop(const std::vector<uint8_t>& bgr, UINT w, UINT h, const int* r, int zoom,
                          UINT* ow, UINT* oh) {
  const int x0 = std::clamp(r[0], 0, static_cast<int>(w) - 1);
  const int y0 = std::clamp(r[1], 0, static_cast<int>(h) - 1);
  const int cw = std::clamp(r[2], 1, static_cast<int>(w) - x0);
  const int ch = std::clamp(r[3], 1, static_cast<int>(h) - y0);
  *ow = cw * zoom;
  *oh = ch * zoom;
  std::vector<uint8_t> out(static_cast<size_t>(*ow) * *oh * 3);
  for (UINT y = 0; y < *oh; ++y)
    for (UINT x = 0; x < *ow; ++x) {
      const size_t s = (static_cast<size_t>(y0 + y / zoom) * w + x0 + x / zoom) * 3;
      std::memcpy(&out[(static_cast<size_t>(y) * *ow + x) * 3], &bgr[s], 3);
    }
  return out;
}

float Mv(const Image& mv, size_t i, int k) { return Half(mv.data.data() + i * 4 + k * 2); }

// Gauss-Jordan with partial pivoting; m and out are rows of a 4x4 matrix.
bool Invert4(const double* m, double* out) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[r * 4 + c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c)
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    const double inv = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c || a[r][c] == 0.0) continue;
      const double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  return true;
}

// The motion the metrics go by, current -> previous in pixels: the camera's,
// from depth and the camera-relative ViewProj of both frame records (in
// double), and the captured vectors on characters. Frame 0 and frames after a
// reset have no previous record and keep the captured vectors.
struct Motion {
  std::vector<float> xy;
  std::vector<uint8_t> object;  // 1 = character
  float X(size_t i) const { return xy[i * 2]; }
  float Y(size_t i) const { return xy[i * 2 + 1]; }
  bool Static(size_t i) const {
    return !object[i] && X(i) * X(i) + Y(i) * Y(i) < 0.05f * 0.05f;
  }
};

bool LoadMotion(const Capture& cap, int t, Motion* m) {
  const UINT w = cap.w, h = cap.h;
  const size_t n = static_cast<size_t>(w) * h;
  Image mv, object, depth;
  if (!ReadDds(cap.File(t, "mv"), &mv) || mv.w != w || mv.h != h) return false;
  const bool have_object = ReadDds(cap.File(t, "object"), &object) && object.w == w;
  m->xy.resize(n * 2);
  m->object.assign(n, 0);
  for (size_t i = 0; i < n; ++i) {
    m->xy[i * 2] = Mv(mv, i, 0);
    m->xy[i * 2 + 1] = Mv(mv, i, 1);
    if (have_object) m->object[i] = object.data[i] > 127;
  }
  const Meta& cur = cap.meta[t];
  if (t == 0 || cur.F("reset") != 0.0f || !ReadDds(cap.File(t, "depth"), &depth) ||
      depth.w != w || depth.h != h)
    return true;
  const Meta& prev = cap.meta[t - 1];
  const float* cur_vp = cur.V("view_proj", 16);
  const float* prev_vp = prev.V("view_proj", 16);
  const float* cur_pos = cur.V("camera_pos", 3);
  const float* prev_pos = prev.V("camera_pos", 3);
  if (!cur_vp || !prev_vp || !cur_pos || !prev_pos) return true;
  double a[16], inv[16], ctp[16];
  for (int i = 0; i < 16; ++i) a[i] = cur_vp[i];
  if (!Invert4(a, inv)) return true;
  // The inverse, then the camera's move, then last frame's ViewProj.
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      inv[r * 4 + c] += (static_cast<double>(cur_pos[r]) - prev_pos[r]) * inv[12 + c];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double s = 0.0;
      for (int k = 0; k < 4; ++k) s += prev_vp[r * 4 + k] * inv[k * 4 + c];
      ctp[r * 4 + c] = s;
    }
  const double jx = 2.0 * cur.F("jitter", 0) / w, jy = -2.0 * cur.F("jitter", 1) / h;
  const float* z = reinterpret_cast<const float*>(depth.data.data());
  for (UINT y = 0; y < h; ++y)
    for (UINT x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      if (m->object[i]) continue;
      const double nx = (x + 0.5) / w * 2.0 - 1.0 - jx, ny = 1.0 - (y + 0.5) / h * 2.0 - jy;
      double p[4];
      for (int r = 0; r < 4; ++r)
        p[r] = ctp[r * 4] * nx + ctp[r * 4 + 1] * ny + ctp[r * 4 + 2] * z[i] + ctp[r * 4 + 3];
      if (p[3] <= 1e-6) {
        m->xy[i * 2] = m->xy[i * 2 + 1] = 0.0f;
        continue;
      }
      m->xy[i * 2] = static_cast<float>((p[0] / p[3] - nx) * 0.5 * w);
      m->xy[i * 2 + 1] = static_cast<float>((p[1] / p[3] - ny) * -0.5 * h);
    }
  return true;
}

// ---- reference and metrics -------------------------------------------------

struct Reference {
  UINT w = 0, h = 0;
  float exposure = 1.0f;
  std::vector<float> y;             // tone-mapped luma of the averaged input
  std::vector<uint8_t> all_static;  // static in every frame
  std::vector<uint8_t> detail;      // the 10% of those with the strongest edges
  double static_share = 0.0;
  std::vector<uint8_t> bgr;  // the averaged input, for ref.png
};

float Laplacian(const std::vector<float>& y, UINT w, UINT h, UINT x, UINT yy) {
  const size_t i = static_cast<size_t>(yy) * w + x;
  if (x == 0 || yy == 0 || x + 1 >= w || yy + 1 >= h) return 0.0f;
  return std::fabs(4.0f * y[i] - y[i - 1] - y[i + 1] - y[i - w] - y[i + w]);
}

bool BuildReference(const Capture& cap, Reference* ref) {
  ref->w = cap.w;
  ref->h = cap.h;
  const size_t n = static_cast<size_t>(cap.w) * cap.h;
  std::vector<double> sum(n * 3, 0.0);
  ref->all_static.assign(n, 1);
  for (int t = 0; t < cap.frames; ++t) {
    Image color;
    if (!ReadDds(cap.File(t, "color"), &color) || color.w != cap.w || color.h != cap.h) return false;
    if (t == 0) {
      // Exposure that puts the scene's log-average luminance at 0.18.
      double log_sum = 0.0;
      float c[3];
      for (size_t i = 0; i < n; ++i) {
        Rgb(color, i, c);
        log_sum += std::log(1e-4 + 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]);
      }
      ref->exposure = std::clamp(static_cast<float>(0.18 / std::exp(log_sum / n)), 0.01f, 20.0f);
    }
    float c[3];
    for (size_t i = 0; i < n; ++i) {
      Rgb(color, i, c);
      for (int k = 0; k < 3; ++k) sum[i * 3 + k] += c[k];
    }
    Motion motion;
    const bool have_mv = LoadMotion(cap, t, &motion);
    for (size_t i = 0; i < n; ++i)
      if (!have_mv || !motion.Static(i)) ref->all_static[i] = 0;
  }
  ref->y.resize(n);
  ref->bgr.resize(n * 3);
  size_t statics = 0;
  for (size_t i = 0; i < n; ++i) {
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = static_cast<float>(sum[i * 3 + k] / cap.frames);
    ref->y[i] = Luma(c, ref->exposure);
    for (int k = 0; k < 3; ++k) ref->bgr[i * 3 + 2 - k] = Srgb8(Tm(c[k] * ref->exposure));
    statics += ref->all_static[i];
  }
  ref->static_share = static_cast<double>(statics) / n;
  // Detail pixels: strongest Laplacian among the always-static ones.
  std::vector<float> edges;
  for (UINT y = 0; y < cap.h; ++y)
    for (UINT x = 0; x < cap.w; ++x)
      if (ref->all_static[static_cast<size_t>(y) * cap.w + x])
        edges.push_back(Laplacian(ref->y, cap.w, cap.h, x, y));
  ref->detail.assign(n, 0);
  if (edges.size() >= 100) {
    std::vector<float> sorted = edges;
    const size_t k = sorted.size() * 9 / 10;
    std::nth_element(sorted.begin(), sorted.begin() + k, sorted.end());
    const float threshold = std::max(sorted[k], 1e-6f);
    for (UINT y = 0; y < cap.h; ++y)
      for (UINT x = 0; x < cap.w; ++x) {
        const size_t i = static_cast<size_t>(y) * cap.w + x;
        if (ref->all_static[i] && Laplacian(ref->y, cap.w, cap.h, x, y) >= threshold)
          ref->detail[i] = 1;
      }
  }
  return true;
}

struct Mean {
  double sum = 0.0;
  size_t count = 0;
  void Add(double v) {
    sum += v;
    ++count;
  }
  double Get() const { return count ? sum / count : -1.0; }
};

class Analyzer {
 public:
  Analyzer(const Reference& ref, int warmup) : ref_(ref), warmup_(warmup) {
    const size_t n = static_cast<size_t>(ref.w) * ref.h;
    sum_.assign(n, 0.0);
    sum_sq_.assign(n, 0.0);
    sum_linear_.assign(n, 0.0);
  }

  // Frame t's output luma, with its motion (see LoadMotion).
  void Add(int t, std::vector<float> y, const Motion& mv) {
    const UINT w = ref_.w, h = ref_.h;
    const size_t n = static_cast<size_t>(w) * h;
    std::vector<uint8_t> statics(n);
    for (size_t i = 0; i < n; ++i) statics[i] = mv.Static(i) ? 1 : 0;
    if (t >= warmup_ && !prev_.empty()) {
      Mean frame_flicker;
      for (UINT py = 0; py < h; ++py)
        for (UINT px = 0; px < w; ++px) {
          const size_t i = static_cast<size_t>(py) * w + px;
          if (statics[i] && prev_static_[i]) {
            const double d = std::fabs(y[i] - prev_[i]);
            flicker_.Add(d);
            frame_flicker.Add(d);
            if (ref_.detail[i]) flicker_detail_.Add(d);
            continue;
          }
          // Moving: last frame's value where this pixel was (bilinear).
          const float sx = px + 0.5f + mv.X(i) - 0.5f, sy = py + 0.5f + mv.Y(i) - 0.5f;
          if (sx < 0.0f || sy < 0.0f || sx > w - 1.0f || sy > h - 1.0f) continue;
          const UINT x0 = static_cast<UINT>(sx), y0 = static_cast<UINT>(sy);
          const UINT x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
          const float fx = sx - x0, fy = sy - y0;
          const float p = (prev_[y0 * w + x0] * (1 - fx) + prev_[y0 * w + x1] * fx) * (1 - fy) +
                          (prev_[y1 * w + x0] * (1 - fx) + prev_[y1 * w + x1] * fx) * fy;
          const double d = std::fabs(y[i] - p);
          if (mv.object[i])
            object_.Add(d);
          else
            moving_.Add(d);
        }
      per_frame_.push_back(frame_flicker.Get());
    }
    if (t >= warmup_) {
      for (size_t i = 0; i < n; ++i) {
        sum_[i] += y[i];
        sum_sq_[i] += static_cast<double>(y[i]) * y[i];
        sum_linear_[i] += y[i] / std::max(1e-6, 1.0 - y[i]);  // undoes Tm
        if (ref_.all_static[i]) {
          const double e = std::fabs(y[i] - ref_.y[i]);
          error_.Add(e);
          if (ref_.detail[i]) error_detail_.Add(e);
        }
      }
      ++frames_;
    }
    prev_ = std::move(y);
    prev_static_ = std::move(statics);
  }

  // Per-pixel std dev over the measured frames.
  float Std(size_t i) const {
    if (!frames_) return 0.0f;
    const double m = sum_[i] / frames_;
    return static_cast<float>(std::sqrt(std::max(0.0, sum_sq_[i] / frames_ - m * m)));
  }

  std::string Report(const char* name) const {
    const UINT w = ref_.w, h = ref_.h;
    const size_t n = static_cast<size_t>(w) * h;
    Mean std_all, std_detail, sharp_out, sharp_ref;
    std::vector<float> stds;
    // The time average is taken in linear light, like the reference.
    std::vector<float> mean_y(n);
    for (size_t i = 0; i < n; ++i)
      mean_y[i] = frames_ ? Tm(static_cast<float>(sum_linear_[i] / frames_)) : 0.0f;
    for (UINT y = 0; y < h; ++y)
      for (UINT x = 0; x < w; ++x) {
        const size_t i = static_cast<size_t>(y) * w + x;
        if (!ref_.all_static[i]) continue;
        const float s = Std(i);
        std_all.Add(s);
        stds.push_back(s);
        if (ref_.detail[i]) {
          std_detail.Add(s);
          sharp_out.Add(Laplacian(mean_y, w, h, x, y));
          sharp_ref.Add(Laplacian(ref_.y, w, h, x, y));
        }
      }
    double p99 = -1.0;
    if (!stds.empty()) {
      const size_t k = stds.size() * 99 / 100;
      std::nth_element(stds.begin(), stds.begin() + k, stds.end());
      p99 = stds[k];
    }
    const double sharp = sharp_ref.Get() > 0 ? sharp_out.Get() / sharp_ref.Get() : -1.0;
    char buf[1024];
    std::string s;
    std::snprintf(buf, sizeof(buf),
                  "run %s\r\nframes_measured %d\r\nstatic %.4f\r\nflicker %.5f\r\nflicker_det %.5f\r\n"
                  "std %.5f\r\nstd_det %.5f\r\nstd_p99 %.5f\r\nerror %.5f\r\nerr_det %.5f\r\n"
                  "sharp %.4f\r\nmoving %.5f\r\nobject %.5f\r\n",
                  name, frames_, ref_.static_share, flicker_.Get(), flicker_detail_.Get(),
                  std_all.Get(), std_detail.Get(), p99, error_.Get(), error_detail_.Get(), sharp,
                  moving_.Get(), object_.Get());
    s = buf;
    s += "flicker_per_frame";
    for (double f : per_frame_) {
      std::snprintf(buf, sizeof(buf), " %.5f", f);
      s += buf;
    }
    s += "\r\n";
    return s;
  }

  // Heat map of the per-pixel std dev on always-static pixels, over a dimmed
  // reference elsewhere.
  std::vector<uint8_t> FlickerImage() const {
    const size_t n = static_cast<size_t>(ref_.w) * ref_.h;
    std::vector<uint8_t> bgr(n * 3);
    for (size_t i = 0; i < n; ++i) {
      if (!ref_.all_static[i]) {
        const uint8_t g = static_cast<uint8_t>(ref_.y[i] * 60.0f);
        bgr[i * 3] = static_cast<uint8_t>(g + 20);
        bgr[i * 3 + 1] = g;
        bgr[i * 3 + 2] = g;
        continue;
      }
      const float v = std::clamp(Std(i) * 12.0f, 0.0f, 1.0f);  // 0.083 std = full
      bgr[i * 3 + 2] = static_cast<uint8_t>(255.0f * std::min(1.0f, v * 2.0f));
      bgr[i * 3 + 1] = static_cast<uint8_t>(255.0f * std::max(0.0f, v * 2.0f - 1.0f));
      bgr[i * 3] = 0;
    }
    return bgr;
  }

 private:
  const Reference& ref_;
  int warmup_;
  int frames_ = 0;
  std::vector<float> prev_;
  std::vector<uint8_t> prev_static_;
  std::vector<double> sum_, sum_sq_, sum_linear_;
  Mean flicker_, flicker_detail_, error_, error_detail_, moving_, object_;
  std::vector<double> per_frame_;
};

bool WriteText(const std::wstring& path, const std::string& text) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"wb") || !f) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return std::fclose(f) == 0 && ok;
}

// Writes the per-run files shared by every kind of run.
void Conclude(const Options& o, const Reference& ref, const Analyzer& a, const char* name,
              const std::wstring& dir, const Image& last, const std::string& extra) {
  WriteText(dir + L"metrics.txt", a.Report(name) + extra);
  WritePng(dir + L"last.png", ref.w, ref.h, ToneMapped(last, ref.exposure));
  const std::vector<uint8_t> flicker = a.FlickerImage();
  WritePng(dir + L"flicker.png", ref.w, ref.h, flicker);
  if (o.has_crop) {
    UINT cw, ch;
    const std::vector<uint8_t> c = Crop(flicker, ref.w, ref.h, o.crop, o.zoom, &cw, &ch);
    WritePng(dir + L"crop_flicker.png", cw, ch, c);
  }
}

void MaybeCrop(const Options& o, const Reference& ref, const Image& img, const std::wstring& dir,
               int t, int frames) {
  if (!o.has_crop || t < frames - o.crop_frames) return;
  UINT cw, ch;
  const std::vector<uint8_t> c =
      Crop(ToneMapped(img, ref.exposure), ref.w, ref.h, o.crop, o.zoom, &cw, &ch);
  wchar_t n[32];
  std::swprintf(n, 32, L"crop_%03d.png", t);
  WritePng(dir + n, cw, ch, c);
}

// ---- runs ------------------------------------------------------------------

int Warmup(const Options& o, const Capture& cap) {
  return o.warmup >= 0 ? std::min(o.warmup, cap.frames - 1) : std::min(16, cap.frames / 2);
}

// input / captured: images straight from the capture.
bool AnalyzeFiles(const Options& o, const Capture& cap, const Reference& ref, const char* run) {
  const bool input = std::strcmp(run, "input") == 0;
  const char* file = input ? "color" : "output";
  const std::wstring dir = o.out + Widen(run) + L"\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  Analyzer a(ref, Warmup(o, cap));
  Image img, mv, object;
  Motion motion;
  for (int t = 0; t < cap.frames; ++t) {
    if (!ReadDds(cap.File(t, file), &img) || !ReadDds(cap.File(t, "mv"), &mv) ||
        !LoadMotion(cap, t, &motion)) {
      std::printf("%s: frame %d has no %s\n", run, t, file);
      return false;
    }
    ReadDds(cap.File(t, "object"), &object);
    a.Add(t, LumaImage(img, ref.exposure), motion);
    MaybeCrop(o, ref, img, dir, t, cap.frames);
  }
  std::string extra;
  if (!input) extra = "backend " + cap.meta[0].text.at("backend") + "\r\n";
  Conclude(o, ref, a, run, dir, img, extra);
  if (input) {
    WritePng(dir + L"ref.png", ref.w, ref.h, ref.bgr);
    // The game's motion vectors of the last frame: |x|, |y| in red, green (4 px = full),
    // characters blue.
    std::vector<uint8_t> bgr(static_cast<size_t>(ref.w) * ref.h * 3);
    for (size_t i = 0; i < static_cast<size_t>(ref.w) * ref.h; ++i) {
      bgr[i * 3 + 2] = static_cast<uint8_t>(std::min(255.0f, std::fabs(Mv(mv, i, 0)) * 64.0f));
      bgr[i * 3 + 1] = static_cast<uint8_t>(std::min(255.0f, std::fabs(Mv(mv, i, 1)) * 64.0f));
      bgr[i * 3] = !object.data.empty() && object.data[i] > 127 ? 255 : 0;
    }
    WritePng(dir + L"mv.png", ref.w, ref.h, bgr);
  }
  return true;
}

HRESULT MakeTexture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind,
                    D3D11_USAGE usage, ComPtr<ID3D11Texture2D>* out) {
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Usage = usage;
  d.BindFlags = bind;
  d.CPUAccessFlags = usage == D3D11_USAGE_STAGING ? D3D11_CPU_ACCESS_READ : 0;
  return device->CreateTexture2D(&d, nullptr, out->GetAddressOf());
}

bool ReadBack(ID3D11DeviceContext* ctx, ID3D11Texture2D* staging, ID3D11Texture2D* src,
              DXGI_FORMAT format, Image* img) {
  ctx->CopyResource(staging, src);
  D3D11_TEXTURE2D_DESC d;
  staging->GetDesc(&d);
  D3D11_MAPPED_SUBRESOURCE m = {};
  if (FAILED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) return false;
  img->format = format;
  img->w = d.Width;
  img->h = d.Height;
  img->bpp = BytesPerPixel(format);
  img->data.resize(static_cast<size_t>(img->w) * img->h * img->bpp);
  for (UINT y = 0; y < img->h; ++y)
    std::memcpy(&img->data[static_cast<size_t>(y) * img->w * img->bpp],
                static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                static_cast<size_t>(img->w) * img->bpp);
  ctx->Unmap(staging, 0);
  return true;
}

// Sets up <out>\<backend>\proxy\ (DLL, DLSS runtime, ini) and replays every frame.
bool ReplayBackend(const Options& o, const Capture& cap, const Reference& ref,
                   const std::string& backend) {
  const std::wstring dir = o.out + Widen(backend) + L"\\";
  const std::wstring proxy_dir = dir + L"proxy\\";
  CreateDirectoryW(dir.c_str(), nullptr);
  CreateDirectoryW(proxy_dir.c_str(), nullptr);
  const std::wstring dll = proxy_dir + L"d3d11.dll";
  if (!CopyFileW(o.dll.c_str(), dll.c_str(), FALSE)) {
    std::printf("%s: cannot copy %s\n", backend.c_str(), Narrow(o.dll).c_str());
    return false;
  }
  const std::wstring dll_dir = o.dll.substr(0, o.dll.find_last_of(L"\\/") + 1);
  CopyFileW((dll_dir + L"nvngx_dlss.dll").c_str(), (proxy_dir + L"nvngx_dlss.dll").c_str(), FALSE);
  const std::wstring ini = proxy_dir + L"DS3_DLSS.ini";
  DeleteFileW(ini.c_str());
  CopyFileW((cap.dir + L"settings.ini").c_str(), ini.c_str(), FALSE);
  auto set = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), ini.c_str());
  };
  set(L"General", L"Enabled", L"1");
  set(L"DLSS", L"Enabled", L"1");
  set(L"Debug", L"DebugView", L"0");
  set(L"Debug", L"CaptureAt", L"0");
  set(L"Debug", L"DumpFrameAt", L"0");
  set(L"Debug", L"DumpShaders", L"0");
  set(L"Widescreen", L"Enabled", L"0");
  for (const std::wstring& s : o.sets) {
    const size_t dot = s.find(L'.'), eq = s.find(L'=');
    if (dot == std::wstring::npos || eq == std::wstring::npos || eq < dot) continue;
    set(s.substr(0, dot).c_str(), s.substr(dot + 1, eq - dot - 1).c_str(), s.substr(eq + 1));
  }

  HMODULE proxy = LoadLibraryW(dll.c_str());
  if (!proxy) {
    std::printf("%s: cannot load the proxy\n", backend.c_str());
    return false;
  }
  auto create_device =
      reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(proxy, "D3D11CreateDevice"));
  auto replay =
      reinterpret_cast<DS3ReplayFrameFn>(GetProcAddress(proxy, "DS3_DLSS_ReplayFrame"));
  if (!create_device || !replay) {
    std::printf("%s: the proxy has no DS3_DLSS_ReplayFrame (too old?)\n", backend.c_str());
    return false;
  }
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> ctx;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;  // what DS3 asks for
  if (FAILED(create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
                           D3D11_SDK_VERSION, &device, nullptr, &ctx)))
    return false;

  const UINT w = cap.w, h = cap.h;
  constexpr UINT kSrv = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> color, depth, objmv, effects, mv_out, bias_out, object_out;
  ComPtr<ID3D11Texture2D> color_rb, mv_rb, bias_rb, object_rb;
  ComPtr<ID3D11ShaderResourceView> objmv_srv, effects_srv;
  bool ok = SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv,
                                  D3D11_USAGE_DEFAULT, &color)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R32G8X24_TYPELESS, kSrv,
                                  D3D11_USAGE_DEFAULT, &depth)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv,
                                  D3D11_USAGE_DEFAULT, &objmv)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, kSrv,
                                  D3D11_USAGE_DEFAULT, &effects)) &&
            SUCCEEDED(device->CreateShaderResourceView(objmv.Get(), nullptr, &objmv_srv)) &&
            SUCCEEDED(device->CreateShaderResourceView(effects.Get(), nullptr, &effects_srv)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R16G16_FLOAT, 0,
                                  D3D11_USAGE_DEFAULT, &mv_out)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R8_UNORM, 0, D3D11_USAGE_DEFAULT,
                                  &bias_out)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R8_UNORM, 0, D3D11_USAGE_DEFAULT,
                                  &object_out)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 0,
                                  D3D11_USAGE_STAGING, &color_rb)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R16G16_FLOAT, 0,
                                  D3D11_USAGE_STAGING, &mv_rb)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R8_UNORM, 0, D3D11_USAGE_STAGING,
                                  &bias_rb)) &&
            SUCCEEDED(MakeTexture(device.Get(), w, h, DXGI_FORMAT_R8_UNORM, 0, D3D11_USAGE_STAGING,
                                  &object_rb));
  if (!ok) return false;

  Analyzer a(ref, Warmup(o, cap));
  Image in, d, out, mv, bias, object, cap_mv, cap_bias, cap_out;
  std::vector<uint8_t> depth_bytes(static_cast<size_t>(w) * h * 8, 0);
  Mean mv_diff, bias_diff, out_diff;
  float mv_max = 0.0f;
  int failed = 0, resets = 0;
  for (int t = 0; t < cap.frames; ++t) {
    const Meta& m = cap.meta[t];
    if (!ReadDds(cap.File(t, "color"), &in) || !ReadDds(cap.File(t, "depth"), &d)) return false;
    ctx->UpdateSubresource(color.Get(), 0, nullptr, in.data.data(), w * 8, 0);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
      std::memcpy(&depth_bytes[i * 8], &d.data[i * 4], 4);
    ctx->UpdateSubresource(depth.Get(), 0, nullptr, depth_bytes.data(), w * 8, 0);
    DS3ReplayFrame f = {};
    f.size = sizeof(f);
    f.color = color.Get();
    f.depth = depth.Get();
    Image extra_img;
    if (ReadDds(cap.File(t, "objmv"), &extra_img)) {
      ctx->UpdateSubresource(objmv.Get(), 0, nullptr, extra_img.data.data(), w * 8, 0);
      f.object_motion = objmv_srv.Get();
    }
    if (ReadDds(cap.File(t, "effects"), &extra_img)) {
      ctx->UpdateSubresource(effects.Get(), 0, nullptr, extra_img.data.data(), w * 8, 0);
      f.effects = effects_srv.Get();
    }
    f.frame = static_cast<uint32_t>(m.F("frame"));
    f.width = m.F("size", 0);
    f.height = m.F("size", 1);
    f.jitter_x = m.F("jitter", 0);
    f.jitter_y = m.F("jitter", 1);
    if (const float* v = m.V("inv_view_proj", 16)) std::memcpy(f.inv_view_proj, v, 64);
    if (const float* v = m.V("view_proj", 16)) std::memcpy(f.view_proj, v, 64);
    if (const float* v = m.V("camera_pos", 3)) std::memcpy(f.camera_pos, v, 12);
    f.proj_z = m.F("proj_z");
    f.near_plane = m.F("near_plane");
    f.fov_y = m.F("fov_y");
    f.frame_ms = m.F("frame_ms");
    f.motion_out = mv_out.Get();
    f.bias_out = bias_out.Get();
    f.object_out = object_out.Get();
    if (!replay(ctx.Get(), &f)) ++failed;
    resets += f.reset;
    if (!ReadBack(ctx.Get(), color_rb.Get(), color.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, &out) ||
        !ReadBack(ctx.Get(), mv_rb.Get(), mv_out.Get(), DXGI_FORMAT_R16G16_FLOAT, &mv) ||
        !ReadBack(ctx.Get(), bias_rb.Get(), bias_out.Get(), DXGI_FORMAT_R8_UNORM, &bias) ||
        !ReadBack(ctx.Get(), object_rb.Get(), object_out.Get(), DXGI_FORMAT_R8_UNORM, &object))
      return false;
    // Replay fidelity: our motion vectors and masks against the game's.
    const bool have_cap_mv = ReadDds(cap.File(t, "mv"), &cap_mv);
    if (have_cap_mv && t > 0) {  // frame 0 resets here but maybe not in game
      for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
        for (int k = 0; k < 2; ++k) {
          const float e = std::fabs(Mv(mv, i, k) - Mv(cap_mv, i, k));
          if (std::isfinite(e)) {
            mv_diff.Add(e);
            mv_max = std::max(mv_max, e);
          }
        }
      if (ReadDds(cap.File(t, "bias"), &cap_bias))
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
          bias_diff.Add(std::abs(bias.data[i] - cap_bias.data[i]) / 255.0);
    }
    if (o.save_dds) {
      wchar_t n[32];
      std::swprintf(n, 32, L"%03d_output.dds", t);
      WriteDds(dir + n, out);
    }
    std::vector<float> y = LumaImage(out, ref.exposure);
    if (t == cap.frames - 1 && cap.meta[t].text.count("backend") &&
        cap.meta[t].text.at("backend") == backend && ReadDds(cap.File(t, "output"), &cap_out)) {
      const std::vector<float> cy = LumaImage(cap_out, ref.exposure);
      for (size_t i = 0; i < y.size(); ++i) out_diff.Add(std::fabs(y[i] - cy[i]));
    }
    Motion motion;
    if (LoadMotion(cap, t, &motion)) a.Add(t, std::move(y), motion);
    MaybeCrop(o, ref, out, dir, t, cap.frames);
  }
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "backend %s\r\nfailed_frames %d\r\nresets %d\r\nmv_vs_game_mean %.6f\r\n"
                "mv_vs_game_max %.6f\r\nbias_vs_game_mean %.6f\r\nlast_vs_game_output %.6f\r\n",
                backend.c_str(), failed, resets, mv_diff.Get(), mv_max, bias_diff.Get(),
                out_diff.Get());
  Conclude(o, ref, a, backend.c_str(), dir, out, buf);
  if (failed) std::printf("%s: %d frames did not run, see %sDS3_DLSS.log\n", backend.c_str(),
                          failed, Narrow(proxy_dir).c_str());
  return failed < cap.frames;
}

bool IsRun(const std::string& s) {
  for (const char* r : kAllRuns)
    if (_stricmp(r, s.c_str()) == 0) return true;
  return false;
}

std::string Canonical(const std::string& s) {
  for (const char* r : kAllRuns)
    if (_stricmp(r, s.c_str()) == 0) return r;
  return s;
}

void PrintTable(const Options& o) {
  const char* const keys[] = {"flicker", "flicker_det", "std", "std_det", "std_p99",
                              "error",   "err_det",     "sharp", "moving", "object"};
  std::printf("\n%-9s", "run");
  for (const char* k : keys) std::printf(" %11s", k);
  std::printf("  replay check\n");
  for (const std::string& run : o.runs) {
    Meta m;
    if (!ReadMeta(o.out + Widen(run) + L"\\metrics.txt", &m)) {
      std::printf("%-9s (no results)\n", run.c_str());
      continue;
    }
    std::printf("%-9s", run.c_str());
    for (const char* k : keys) std::printf(" %11.5f", m.F(k));
    if (m.values.count("mv_vs_game_mean"))
      std::printf("  mv %.4f px, failed %d", m.F("mv_vs_game_mean"),
                  static_cast<int>(m.F("failed_frames")));
    std::printf("\n");
  }
  Meta m;
  if (ReadMeta(o.out + L"input\\metrics.txt", &m))
    std::printf("static pixels: %.1f%% of the frame, %d frames measured\n", 100.0 * m.F("static"),
                static_cast<int>(m.F("frames_measured")));
  std::printf("lower is better except sharp (1 = as sharp as the averaged input); "
              "-1 = not measurable here. Files: %s\n",
              Narrow(o.out).c_str());
}

int Usage() {
  std::printf(
      "usage: capture_replay <capture folder> [input captured DLSS] [--dll path]\n"
      "       [--out folder] [--warmup n] [--set Section.Key=Value] [--crop x,y,w,h]\n"
      "       [--zoom n] [--crop-frames n] [--save-dds]\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  InitHalf();
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Options o;
  std::vector<std::wstring> pass_through;  // for the child processes
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
    if (a == L"--child") {
      o.child = Narrow(next());
      continue;
    }
    if (a == L"--dll") {
      o.dll = next();
    } else if (a == L"--out") {
      o.out = next();
    } else if (a == L"--warmup") {
      o.warmup = _wtoi(next().c_str());
    } else if (a == L"--set") {
      o.sets.push_back(next());
    } else if (a == L"--crop") {
      o.has_crop = swscanf_s(next().c_str(), L"%d,%d,%d,%d", &o.crop[0], &o.crop[1], &o.crop[2],
                             &o.crop[3]) == 4;
    } else if (a == L"--zoom") {
      o.zoom = std::max(1, _wtoi(next().c_str()));
    } else if (a == L"--crop-frames") {
      o.crop_frames = std::max(1, _wtoi(next().c_str()));
    } else if (a == L"--save-dds") {
      o.save_dds = true;
    } else if (a.rfind(L"--", 0) == 0) {
      return Usage();
    } else if (o.capture.empty()) {
      o.capture = a;
    } else if (IsRun(Narrow(a))) {
      o.runs.push_back(Canonical(Narrow(a)));
      continue;
    } else {
      return Usage();
    }
    pass_through.push_back(a);
    if (a == L"--dll" || a == L"--out" || a == L"--warmup" || a == L"--set" || a == L"--crop" ||
        a == L"--zoom" || a == L"--crop-frames")
      pass_through.push_back(argv[i]);
  }
  if (o.capture.empty()) return Usage();
  if (o.runs.empty())
    for (const char* r : kAllRuns) o.runs.push_back(r);
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (o.dll.empty()) {
    o.dll = self;
    o.dll = o.dll.substr(0, o.dll.find_last_of(L'\\') + 1) + L"d3d11.dll";
  }
  wchar_t full[MAX_PATH];
  GetFullPathNameW(o.dll.c_str(), MAX_PATH, full, nullptr);
  o.dll = full;
  Capture cap;
  if (!OpenCapture(o.capture, &cap)) {
    std::printf("no capture frames in %s\n", Narrow(o.capture).c_str());
    return 1;
  }
  if (o.out.empty()) o.out = cap.dir + L"replay";
  for (wchar_t& c : o.out)
    if (c == L'/') c = L'\\';
  if (o.out.back() != L'\\') o.out += L'\\';
  // Every missing folder of the path, not just the last one.
  for (size_t i = o.out.find(L'\\', 3); i != std::wstring::npos; i = o.out.find(L'\\', i + 1))
    CreateDirectoryW(o.out.substr(0, i).c_str(), nullptr);

  Reference ref;
  if (!BuildReference(cap, &ref)) {
    std::printf("cannot read the capture's colour frames\n");
    return 1;
  }
  if (!o.child.empty()) return ReplayBackend(o, cap, ref, o.child) ? 0 : 1;

  std::printf("%d frames of %ux%u, %d warm-up, %.1f%% static pixels, captured with %s\n",
              cap.frames, cap.w, cap.h, Warmup(o, cap), 100.0 * ref.static_share,
              cap.meta[0].text.count("backend") ? cap.meta[0].text.at("backend").c_str() : "?");
  for (const std::string& run : o.runs) {
    std::printf("%s...\n", run.c_str());
    std::fflush(stdout);
    if (run == "input" || run == "captured") {
      AnalyzeFiles(o, cap, ref, run.c_str());
      continue;
    }
    // The replay runs in its own process, with its own copy of the proxy.
    std::wstring cmd = L"\"" + std::wstring(self) + L"\" --child " + Widen(run);
    for (const std::wstring& p : pass_through) cmd += L" \"" + p + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
      std::printf("%s: cannot start the replay process\n", run.c_str());
      continue;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code) std::printf("%s: replay failed (exit %lu)\n", run.c_str(), code);
  }
  PrintTable(o);
  return 0;
}
