#include "tweaks/frame_rate.h"

#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "common/config.h"
#include "core.h"

namespace ds3::frame_rate {
namespace {

using game_memory::Range;

// --- sprint fix ---

using SprintCheckFn = void (*)(void* self, float frame_seconds);
SprintCheckFn o_SprintCheck = nullptr;
SprintFactors* g_factors = nullptr;  // within 2 GB of the game code; the check reads it
std::atomic<bool> g_logged_frame_time{false};

void Hk_SprintCheck(void* self, float frame_seconds) {
  *g_factors = ForFrameTime(frame_seconds);
  if (!g_logged_frame_time.exchange(true))
    Log("frame rate: sprint check running, frame time %.2f ms -> factors %.2f %.4f %.4f",
        frame_seconds * 1000.0f, g_factors->speed_scale, g_factors->slowdown,
        g_factors->recovery);
  o_SprintCheck(self, frame_seconds);
}

float ReadFloat(uint8_t* disp) {
  float value = 0.0f;
  std::memcpy(&value, game_memory::RipTarget(disp), sizeof(value));
  return value;
}

void InstallSprintFix(Range code) {
  const SprintSite site = FindSprintCheck(code);
  if (!site.function) {
    Log("frame rate: sprint check not found (other game version?), sprint fix skipped");
    return;
  }
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  g_factors = static_cast<SprintFactors*>(game_memory::AllocateNear(site.function, 4096));
  if (!g_factors) {
    Log("frame rate: no memory near the game code, sprint fix skipped");
    return;
  }
  *g_factors = ForFrameTime(0.0f);  // 60 fps values until the first call

  const MH_STATUS init = MH_Initialize();
  MH_STATUS status = init == MH_OK || init == MH_ERROR_ALREADY_INITIALIZED
                         ? MH_CreateHook(site.function, reinterpret_cast<void*>(&Hk_SprintCheck),
                                         reinterpret_cast<void**>(&o_SprintCheck))
                         : init;
  if (status != MH_OK) {
    Log("frame rate: sprint check hook failed: %s", MH_StatusToString(status));
    return;
  }
  // The operands first (with 60 fps values in place the game behaves as before),
  // then the hook that keeps them up to date.
  game_memory::Redirect(site.speed_disp, &g_factors->speed_scale);
  game_memory::Redirect(site.slowdown_disp, &g_factors->slowdown);
  game_memory::Redirect(site.recovery_disp, &g_factors->recovery);
  status = MH_EnableHook(site.function);
  Log("frame rate: sprint fix at exe+0x%llX, factors at %p: %s",
      static_cast<unsigned long long>(site.function - base), static_cast<void*>(g_factors),
      MH_StatusToString(status));
}

// --- unlock ---

bool InModule(uintptr_t address, uint8_t* base) {
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
  return address >= reinterpret_cast<uintptr_t>(base) &&
         address < reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
}

DWORD WINAPI Worker(void*) {
  const Config& config = GetConfig();
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
  const Range code = game_memory::CodeSection(base);
  if (!HasDebugLimiter(code)) {
    Log("frame rate: frame limiter layout not recognised (other game version?), nothing changed");
    return 0;
  }
  uint8_t** slot = FindFlipperSlot(code);
  if (!slot) {
    Log("frame rate: flip object not found (other game version?), nothing changed");
    return 0;
  }

  // The game creates the object early during start-up; wait for it.
  uint8_t* flipper = nullptr;
  for (int i = 0; i < 1200 && !flipper; ++i) {
    uint8_t* object = *reinterpret_cast<uint8_t* volatile*>(slot);
    if (object && InModule(*reinterpret_cast<uintptr_t*>(object), base))
      flipper = object;
    else
      Sleep(100);
  }
  if (!flipper) {
    Log("frame rate: flip object never appeared, nothing changed");
    return 0;
  }

  float* target = reinterpret_cast<float*>(flipper + kFlipperTargetFps);
  bool* use_target = reinterpret_cast<bool*>(flipper + kFlipperUseTarget);
  Log("frame rate: flip object %p (slot exe+0x%llX), was %.1f fps / debug limiter %d", flipper,
      static_cast<unsigned long long>(reinterpret_cast<uint8_t*>(slot) - base), *target,
      *use_target);
  *target = config.frame_rate_fps;
  *use_target = true;
  Log("frame rate: unlocked, cap %.1f fps", config.frame_rate_fps);

  if (config.frame_rate_fps > kSprintFixAbove)
    InstallSprintFix(code);
  else
    Log("frame rate: cap at or below %.0f fps, no sprint fix needed (game code untouched)",
        kSprintFixAbove);
  return 0;
}

}  // namespace

SprintFactors ForFrameTime(float seconds) {
  if (!(seconds > 0.0f)) return {30.0f, 0.8f, 1.2f};
  seconds = std::clamp(seconds, 0.001f, 0.05f);
  const float frames = seconds * 60.0f;  // in 60 fps frames
  return {0.5f / seconds, std::pow(0.8f, frames), std::pow(1.2f, frames)};
}

SprintSite FindSprintCheck(Range code) {
  // addss; shufps; sqrtps; mulss xmm0, [30.0]; comiss  (the "stuck" distance test)
  static const uint8_t kAnchor[] = {0xF3, 0x0F, 0x58, 0x00, 0x0F, 0xC6, 0x00, 0x00, 0x0F, 0x51, 0x00,
                                    0xF3, 0x0F, 0x59, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x2F};
  static const uint8_t kPrologue[] = {0x4C, 0x8B, 0xDC, 0x57, 0x48, 0x83, 0xEC, 0x70};
  static const uint8_t kMulss[] = {0xF3, 0x0F, 0x59, 0x05};
  uint8_t* anchor = game_memory::Find(code, kAnchor, "xxx?xx??xx?xxxx????xx");
  if (!anchor || game_memory::Find(code, kAnchor, "xxx?xx??xx?xxxx????xx", anchor + 1))
    return {};  // missing or ambiguous

  SprintSite site;
  site.speed_disp = anchor + 15;
  site.slowdown_disp = anchor + 0x2D;
  site.recovery_disp = anchor + 0x58;
  if (std::memcmp(site.slowdown_disp - 4, kMulss, 4) != 0 ||
      std::memcmp(site.recovery_disp - 4, kMulss, 4) != 0 || ReadFloat(site.speed_disp) != 30.0f ||
      ReadFloat(site.slowdown_disp) != 0.8f || ReadFloat(site.recovery_disp) != 1.2f)
    return {};
  for (int back = 8; back <= 0x180 && !site.function; ++back)
    if (anchor - back >= code.begin && std::memcmp(anchor - back, kPrologue, sizeof(kPrologue)) == 0)
      site.function = anchor - back;
  if (!site.function) return {};
  return site;
}

uint8_t** FindFlipperSlot(Range code) {
  // mov ecx, 0x368 (object size); call operator new; ... mov [rip + slot], rax
  static const uint8_t kNew[] = {0xB9, 0x68, 0x03, 0x00, 0x00, 0xE8};
  static const uint8_t kStore[] = {0x48, 0x89, 0x05};
  uint8_t** found = nullptr;
  for (uint8_t* hit = game_memory::Find(code, kNew, "xxxxxx"); hit;
       hit = game_memory::Find(code, kNew, "xxxxxx", hit + 1)) {
    for (int i = 10; i + 7 <= 64; ++i) {
      if (std::memcmp(hit + i, kStore, sizeof(kStore)) != 0) continue;
      auto** slot = reinterpret_cast<uint8_t**>(game_memory::RipTarget(hit + i + 3));
      if (found && found != slot) return nullptr;  // two different objects: not sure
      found = slot;
      break;
    }
  }
  return found;
}

bool HasDebugLimiter(Range code) {
  static const uint8_t kUseTarget[] = {0x38, 0x00, 0x58, 0x03, 0x00, 0x00};       // cmp [r+0x358], r8b
  static const uint8_t kDivTarget[] = {0xF3, 0x0F, 0x5E, 0x00, 0x54, 0x03, 0x00, 0x00};  // divss x, [r+0x354]
  // The flag test sits just before the division.
  for (uint8_t* div = game_memory::Find(code, kDivTarget, "xxx?xxxx"); div;
       div = game_memory::Find(code, kDivTarget, "xxx?xxxx", div + 1)) {
    if (div - 0x20 >= code.begin && game_memory::Find({div - 0x20, div}, kUseTarget, "x?xxxx"))
      return true;
  }
  return false;
}

void Apply() {
  const Config& config = GetConfig();
  if (!config.frame_rate_unlock) return;
  if (game_memory::Ds3DebugFpsInstalled()) {
    Log("frame rate: DS3DebugFps is installed (D3DCompiler_43.dll + FPSconfig.ini), it does "
        "the same: [FrameRate] skipped. Remove those two files to use ours.");
    return;
  }
  if (HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(thread);
}

}  // namespace ds3::frame_rate
