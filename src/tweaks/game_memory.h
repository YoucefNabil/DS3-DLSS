#pragma once
// Small helpers for the [FrameRate] and [Window] tweaks: finding code in
// DarkSoulsIII.exe, patching it, and redirecting the exe's own imports.
#include <cstddef>
#include <cstdint>

namespace ds3::game_memory {

struct Range {
  uint8_t* begin = nullptr;
  uint8_t* end = nullptr;
};

// The first .text section of the module at base (the game's code; v1.15.2
// has a second one for the DRM stub, which is left out).
Range CodeSection(uint8_t* base);

// First match of pattern in range; mask has 'x' for bytes that must match and
// '?' for wildcards (same length as pattern). nullptr when not found.
uint8_t* Find(Range range, const uint8_t* pattern, const char* mask, uint8_t* from = nullptr);

// Target of the rip-relative operand whose 4-byte displacement is at disp
// and whose instruction ends at disp + 4 + tail (tail = immediate bytes).
inline uint8_t* RipTarget(uint8_t* disp, int tail = 0) {
  return disp + 4 + tail + *reinterpret_cast<int32_t*>(disp);
}

// Writes bytes into code (or read-only data) and flushes the instruction cache.
void Write(void* address, const void* bytes, size_t size);

// Points the rip-relative operand at disp to target. False when target is
// out of +-2 GB reach.
bool Redirect(uint8_t* disp, const void* target);

// size bytes of read/write memory within 2 GB of near (for Redirect).
void* AllocateNear(const void* near_address, size_t size);

// Replaces the exe's import of dll!function with hook; the old pointer goes to
// *original. Only calls made by the exe itself are affected.
bool HookImport(uint8_t* base, const char* dll, const char* function, void* hook,
                void** original);

// True when DS3DebugFps (D3DCompiler_43.dll + FPSconfig.ini next to the exe)
// is installed: it patches the same code, so the tweaks stand down.
bool Ds3DebugFpsInstalled();

}  // namespace ds3::game_memory
