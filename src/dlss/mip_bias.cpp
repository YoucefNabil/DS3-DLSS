#include "dlss/mip_bias.h"

#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>

#include "common/config.h"
#include "common/log.h"

namespace ds3::mip_bias {
namespace {

using Microsoft::WRL::ComPtr;

// {5E0B3C7A-6D2F-4B8E-9A41-3C1D7F2B9E60}: private data slot on the original
// sampler holding a Watcher, released by the runtime when the sampler dies.
constexpr GUID kWatcherGuid = {0x5e0b3c7a, 0x6d2f, 0x4b8e,
                               {0x9a, 0x41, 0x3c, 0x1d, 0x7f, 0x2b, 0x9e, 0x60}};

std::shared_mutex g_mutex;
std::unordered_map<ID3D11SamplerState*, ComPtr<ID3D11SamplerState>> g_twins;
std::atomic<uint32_t> g_twin_count{0};

std::atomic<bool> g_enabled{true};  // hotkey
std::atomic<bool> g_now{false};     // swap samplers right now

// Forgets the twin when the original sampler is destroyed, so a new sampler
// that happens to get the same address is never mistaken for it.
class Watcher final : public IUnknown {
 public:
  explicit Watcher(ID3D11SamplerState* original) : original_(original) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (!out) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
      *out = static_cast<IUnknown*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = --refs_;
    if (left == 0) {
      ComPtr<ID3D11SamplerState> twin;  // released after the lock
      {
        std::unique_lock lock(g_mutex);
        const auto it = g_twins.find(original_);
        if (it != g_twins.end()) {
          twin = std::move(it->second);
          g_twins.erase(it);
          --g_twin_count;
        }
      }
      delete this;
    }
    return left;
  }

 private:
  ID3D11SamplerState* original_;  // identity only
  std::atomic<ULONG> refs_{1};
};

}  // namespace

bool WantsTwin(const D3D11_SAMPLER_DESC& desc, D3D11_SAMPLER_DESC* biased) {
  const float bias = GetConfig().dlss_mip_bias;
  if (!GetConfig().dlss || bias == 0.0f) return false;
  // Shadow (comparison) and min/max samplers read data, not colour.
  if ((desc.Filter & 0x180) != 0) return false;
  // Point samplers fetch exact texels (lookups, full-screen passes).
  if (desc.Filter == D3D11_FILTER_MIN_MAG_MIP_POINT) return false;
  // Samplers limited to the top mip have nothing to bias.
  if (desc.MaxLOD <= 0.0f || desc.MaxLOD <= desc.MinLOD) return false;
  *biased = desc;
  biased->MipLODBias = std::clamp(desc.MipLODBias + bias, -16.0f, 15.99f);
  return true;
}

void Register(ID3D11SamplerState* original, ID3D11SamplerState* twin) {
  {
    std::unique_lock lock(g_mutex);
    // The runtime returns the same object for identical descs.
    if (!g_twins.try_emplace(original, twin).second) return;
  }
  auto* watcher = new Watcher(original);
  if (FAILED(original->SetPrivateDataInterface(kWatcherGuid, watcher))) {
    // Without a watcher the address could be reused; do not keep the twin.
    std::unique_lock lock(g_mutex);
    g_twins.erase(original);
    watcher->Release();
    return;
  }
  watcher->Release();  // the sampler holds it now
  const uint32_t n = ++g_twin_count;
  if (n <= 3 || n % 50 == 0) {
    D3D11_SAMPLER_DESC d;
    original->GetDesc(&d);
    Log("mip bias: %u samplers with a biased twin (latest filter=0x%X bias %.2f)", n, d.Filter,
        d.MipLODBias + GetConfig().dlss_mip_bias);
  }
}

void Toggle() {
  const bool on = !g_enabled.load();
  g_enabled = on;
  Log("mip bias: %s (hotkey)", on ? "on" : "off");
}

void OnPresent(bool running) {
  g_now.store(running && g_enabled.load(std::memory_order_relaxed) &&
                  GetConfig().dlss_mip_bias != 0.0f,
              std::memory_order_relaxed);
}

void OnSceneDone() { g_now.store(false, std::memory_order_relaxed); }

ID3D11SamplerState* const* Substitute(UINT count, ID3D11SamplerState* const* samplers,
                                      ID3D11SamplerState** scratch) {
  if (!g_now.load(std::memory_order_relaxed) || !samplers || count == 0 ||
      count > D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
    return samplers;
  bool changed = false;
  std::shared_lock lock(g_mutex);
  for (UINT i = 0; i < count; ++i) {
    scratch[i] = samplers[i];
    if (!samplers[i]) continue;
    const auto it = g_twins.find(samplers[i]);
    if (it == g_twins.end()) continue;
    scratch[i] = it->second.Get();
    changed = true;
  }
  return changed ? scratch : samplers;
}

uint32_t TwinCount() { return g_twin_count.load(); }

}  // namespace ds3::mip_bias
