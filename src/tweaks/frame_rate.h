#pragma once
// [FrameRate]: frame rate unlock and the sprint fix that goes with it.
//
// Unlock: the game has a debug frame limiter in its flip object (the one
// DS3DebugFps uses): a flag at +0x358 switches the 60 fps cap to a target of
// 1 / (float at +0x354) seconds per frame, with vsync interval 0. We set those
// two fields once the object exists; no game code changes.
//
// Sprint fix (automatic, only when the cap is above kSprintFixAbove fps, where
// the bug shows; at or below it no game code is changed): a per-character
// check (exe+0x5C72B0 on v1.15.2) decides "the player is stuck while
// sprinting" when it moved less than 1/30 of a unit in one frame, then scales the movement speed by 0.8 per frame (down to 0.5),
// or by 1.2 per frame back up to 1. The game passes it the frame time, but it
// ignores it: everything is per frame, tuned for 60 fps. Above ~82 fps the
// per-frame distance of a normal sprint start falls under the threshold and
// sprinting slows down. We hook the function, and its three constants are
// recomputed from the frame time the game passes in, so the check behaves
// exactly as it does at 60 fps (2 units/s, 0.8 and 1.2 per 1/60 s), at any
// frame rate. (DS3DebugFps estimates the frame time from the flip object and
// adds fudge factors; with the real frame time none are needed.)
#include <cstdint>

#include "tweaks/game_memory.h"

namespace ds3::frame_rate {

// Called from DllMain. Does nothing unless [FrameRate] Unlock=1.
void Apply();

// --- exposed for tests ---

struct SprintFactors {
  float speed_scale;  // distance moved this frame * speed_scale < 1 means "stuck"
  float slowdown;     // speed multiplier per frame while stuck
  float recovery;     // speed multiplier per frame otherwise
};
// 60 fps values: 30, 0.8, 1.2. A frame time outside 1..50 ms is clamped; zero
// or invalid gives the 60 fps values.
SprintFactors ForFrameTime(float seconds);

struct SprintSite {
  uint8_t* function = nullptr;       // start of the check (MinHook target)
  uint8_t* speed_disp = nullptr;     // rip displacement of the 30.0 operand
  uint8_t* slowdown_disp = nullptr;  // ... of 0.8
  uint8_t* recovery_disp = nullptr;  // ... of 1.2
};
// Finds the check and verifies its three constants. All null when not found.
SprintSite FindSprintCheck(game_memory::Range code);

// Global slot that holds the flip object pointer, or nullptr.
uint8_t** FindFlipperSlot(game_memory::Range code);

// True when the limiter code reads the fields at +0x354 / +0x358 (same layout
// as v1.15.2).
bool HasDebugLimiter(game_memory::Range code);

// Caps above this get the sprint fix. The slowdown starts around 82 fps.
inline constexpr float kSprintFixAbove = 80.0f;

inline constexpr size_t kFlipperTargetFps = 0x354;   // float
inline constexpr size_t kFlipperUseTarget = 0x358;   // bool

}  // namespace ds3::frame_rate
