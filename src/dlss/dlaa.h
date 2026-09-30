#pragma once
#include <d3d11.h>

// DLAA on DS3's HDR scene: NVIDIA NGX (DLSS) evaluated at native resolution
// right before the game's first post-process pass, with motion vectors
// reconstructed from depth and the camera matrices. Everything goes through
// the D3D11 API on the game's immediate context; game memory is not touched.
namespace ds3::dlaa {

// ClearDepthStencilView hook (any context): tracks the scene depth buffer.
void OnClearDepth(ID3D11DepthStencilView* dsv);

// Before every Draw. Runs DLAA once per frame, just before PS 393A947C35535471
// (the first post-process pass) reads the HDR scene.
void BeforeDraw(ID3D11DeviceContext* ctx);

void Toggle();     // hotkey: DLAA + jitter on/off
void OnPresent();  // updates whether the camera should be jittered

}  // namespace ds3::dlaa
