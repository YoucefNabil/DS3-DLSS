#pragma once
// Capture mode ([Debug] CaptureKey / CaptureAt): saves N consecutive frames of
// everything DLSS gets, so tools/capture_replay can run it on real game
// frames offline.
//
// Each frame is copied on the GPU into one of a few staging sets, read back a
// few frames later and written by a background thread, so the captured frames
// stay consecutive even when the disk is slow (the game then waits instead).
// Files, in DS3_DLSS_capture_<date>_<time>\ next to the DLL:
//   NNN_color.dds    scene colour going into DLSS (RGBA16F, jittered)
//   NNN_depth.dds    scene depth (R32F, standard Z, 1 = far)
//   NNN_objmv.dds    per-object motion target (RGBA16F), when ObjectMotion ran
//   NNN_effects.dds  what blended effects added (RGBA16F), when Reactive ran
//   NNN_mv.dds       motion vectors DLSS got (R16G16F, pixels)
//   NNN_bias.dds     reactive mask (R8), NNN_object.dds object mask (R8)
//   NNN_output.dds   the colour written back into the scene (RGBA16F)
//   NNN.txt          camera, jitter, reset, frame time and pass settings
//   settings.ini     the DS3_DLSS.ini the game ran with
//   capture.txt      summary
// Immediate context only.

#include <d3d11.h>

#include <cstdint>

namespace ds3::capture {

enum Slot {
  kColor,
  kDepth,
  kObjectMotion,
  kEffects,
  kMotion,
  kBias,
  kObject,
  kOutput,
  kSlotCount
};

void Request();      // hotkey: start a capture at the next evaluated frame
bool Active();       // a capture is running: call Grab/Finish for this frame
// Starts if requested (or at frame `at` of [Debug] CaptureAt).
void BeginFrame(uint32_t frame);
// Copies `source` (or nothing when null) into this frame's slot.
void Grab(ID3D11DeviceContext* ctx, Slot slot, ID3D11Resource* source);
// Ends the frame with its text record; reads back older frames.
void Finish(ID3D11DeviceContext* ctx, const char* meta);

}  // namespace ds3::capture
