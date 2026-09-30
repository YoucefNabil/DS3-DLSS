#pragma once
#include <d3d11.h>

#include <atomic>

// Debug tool for Phase 1: records one frame's worth of immediate-context work
// (draws, dispatches, copies, clears) with shader hashes and render-target
// descriptions, and writes it to DS3_DLSS_frame_<time>.txt next to the DLL.
// Deferred contexts are recorded from one frame earlier, so command lists built
// ahead of time are known; each list's work is spliced in where it executes.
namespace ds3::frame_dump {

extern std::atomic<bool> g_active;  // checked on every draw; cheap when false

inline bool Active() { return g_active.load(std::memory_order_relaxed); }

void Request();
void OnPresent();  // starts a requested capture, or finishes the running one

void RecordDraw(ID3D11DeviceContext* ctx, const char* op, UINT count, UINT instances);
void RecordDispatch(ID3D11DeviceContext* ctx, UINT x, UINT y, UINT z);
void RecordCopy(ID3D11DeviceContext* ctx, const char* op, ID3D11Resource* dst, ID3D11Resource* src);
void RecordClear(ID3D11DeviceContext* ctx, const char* op, ID3D11View* view);
void RecordExecuteCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list);
void RecordFinishCommandList(ID3D11DeviceContext* ctx, ID3D11CommandList* list);
// Constant-buffer uploads. Only large constant buffers (camera/scene sized) are
// recorded, with the matrices decoded using the cbSceneParam layout.
void RecordMap(ID3D11DeviceContext* ctx, ID3D11Resource* r, D3D11_MAP type, void* data);
void RecordUnmap(ID3D11DeviceContext* ctx, ID3D11Resource* r);
void RecordUpdate(ID3D11DeviceContext* ctx, const char* op, ID3D11Resource* r,
                  const D3D11_BOX* box, const void* data);

}  // namespace ds3::frame_dump
