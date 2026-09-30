#pragma once
// Negative texture LOD bias while DLAA runs. DLSS rebuilds detail from many
// jittered frames, so textures can be sampled from a sharper mip than the game
// would pick; NVIDIA recommends log2(render / output) - 1, i.e. -1 for DLAA.
// Every sampler that uses mips gets a twin with the bias added
// ([DLSS] MipBias). PSSetSamplers binds the twin instead, from Present until
// DLAA runs (the 3D scene), so post-processing and the UI keep the originals.
// Only D3D11 state objects are involved; game memory is not touched.

#include <d3d11.h>

#include <cstdint>

namespace ds3::mip_bias {

// Whether `desc` should get a twin; fills `biased` with the twin's desc.
bool WantsTwin(const D3D11_SAMPLER_DESC& desc, D3D11_SAMPLER_DESC* biased);
// After CreateSamplerState: remembers the twin until the original is destroyed.
void Register(ID3D11SamplerState* original, ID3D11SamplerState* twin);

void Toggle();                // hotkey: bias on/off (while DLAA runs)
void OnPresent(bool running);  // a new frame starts; running = DLAA runs this frame
void OnSceneDone();           // DLAA ran: the rest of the frame is post-processing and UI

// PSSetSamplers hook, any thread. Returns the samplers to bind: `samplers`
// itself when nothing changes, else `scratch` (at least `count` entries).
ID3D11SamplerState* const* Substitute(UINT count, ID3D11SamplerState* const* samplers,
                                      ID3D11SamplerState** scratch);

uint32_t TwinCount();

}  // namespace ds3::mip_bias
