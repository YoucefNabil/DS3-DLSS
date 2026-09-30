# DS3 frame analysis (v1.15.2, 1920x1080, D3D11 FL 11_1)

This analysis comes from `DS3_DLSS_frame_20260930_173222.txt` (in-game AA on) and `..._173230.txt` (AA off).
Both dumps include deferred command lists, and every shader was dumped with `DumpShaders=1`.
Resource ids (`Tn`) are per-dump, so the ids below refer to the AA-off dump.
Shader hashes are the DXBC checksums and are stable across runs.

## Frame structure

The game records almost everything on 17 deferred contexts. The immediate context runs 64 command lists in order.

| Stage | Where | Targets | Notes |
|---|---|---|---|
| Shadow maps | L13–L19 | DS T1 (8192² R16), T59 (512²) | Cascades use 4096² viewports |
| G-buffer | L22–L37 | RT T21 RGBA16F, T22 RGBA8, T23 RGBA8, T24 RGBA8, T25 R11G11B10F, T6 RGBA16F; DS **T0** (R32G8X24, D32S8) | Only equipment shaders write target 5 (T6): material/glow data, **not velocity** |
| Depth copies | L37, L46, L53 | T2 ← T0 (CopyResource) | T2 is the readable depth copy |
| Half-res depth, AO | L38 | T3 960x540, T14 480x270; PS 68FD/0D1D/ECEA | AO is temporal (uses `g_mCurToPrevScreen`) |
| Deferred lighting | L39–L42 | RT T53+T40 / T39 / T60 | |
| Lit HDR scene | L43–L53 | **T5 RGBA16F** (+T6) DS T0 | Forward/transparent passes draw into T5 |
| Half-res transparents | L54 | T15 960x540 → composited into T5 (PS ACF6) | |
| Motion-blur velocity | L55–L56 | T493 960x540 RGBA8, DS T3 | See "Velocity" below |
| Post HDR | immediate, #1397 on | PS 393A copies T5 → T692 (RGBA16_UNORM); 7004 fog/scatter; bloom/glare pyramid; motion blur D55E/E484 | |
| Tonemap | PS C2EF3668568C5617 | → RGBA8 | |
| LDR copy | PS B294B3B1663E3155 | → T593 (backbuffer-sized RGBA8) | |
| **In-game AA** | L60 | PS C78BA12ACC57901B (copy T619→T620), then **PS CAA861F2CF791746** (T620→T619) | Only present with AA on |
| LDR glow | L60 | CC7E reads T6, pyramid B9FD/5584, composite into the LDR target | |
| UI/HUD | immediate, after L62 | into the LDR target, DS T0 | |

## In-game AA

The in-game AA is `PS CAA861F2CF791746`: a spatial edge filter that runs after tonemapping on the RGBA8 image.
- It samples 4 neighbours, computes Rec.601 luma (0.299/0.587/0.114), and blends 1/6-weighted edge taps.
- It has no temporal component, which explains why the shimmer survives.
- With DLAA the in-game AA should be set to **Off**. The pass then disappears by itself, with no need to skip it by hash.

## Velocity

- There is **no full-resolution velocity buffer**.
- Motion blur builds `T493`, a half-res RGBA8 buffer:
  - moving objects are drawn with VS C48361A1 / 96632548 / F54DC448 (`VC_NowToPrevView`, `VC_PrevWorld`, `cbPrevObjMatrix`);
  - `PS 33B0AAA91300023E` then dilates it (`g_VelocityTex`, keeps the nearest-depth neighbour).
- Encoding: `velocity = rg * 0.4 - 0.2` (UV units), `b` = depth.
- At 8 bits that is ~0.0016 UV per step (~3 px at 1080p), too coarse for DLSS.
- **Consequence:** DLSS motion vectors are computed by us from depth (T0/T2) with the current and previous camera matrices (camera motion).
  - Later, T493 may serve as an optional coarse correction for characters.

## Camera constants: `cbSceneParam` (register **b8**, VS and PS)

This constant buffer is shared by ~3150 shaders. Offsets come from the shader reflection data:

| Offset | Member |
|---|---|
| 0 | float4 FC_FarClipInfo |
| 16 | float4 FC_ScreenSize |
| 32 | float4 FC_DepthComputeParam |
| 48 | float4 SC_CameraPos |
| 64 | float4 VC_ClipPlane |
| 80 | float4x3 FC_MatrixView |
| 128 | float4x4 FC_MatrixInvViewProj |
| 192 | float4x4 FC_MatrixInvProj |
| 256 | float4x4 FC_MatrixProj |
| 320 | float4x4 FC_MatrixInvView |
| 384 | float4x4 VC_MatrixViewProj (geometry VS use this) |
| 448.. | shadow params, `FC_ProjSpaceToShadowMatrix[4]` @608, light tiles, cloud shadows (ends at 952) |

Other constant buffers: cb4 `cbInstanceData` (per-instance world 4x3) and cb5 `cbObjMatrix`.

### Jitter plan (replaces the camera code patch)
The jitter would be added by adjusting `VC_MatrixViewProj` / `FC_MatrixProj` and their inverses **in the cbSceneParam upload** (Map/Unmap or UpdateSubresource on that buffer).
- This involves no game memory writes and no executable patch. It is the same class of interception as the rest of the proxy.
- Only the main camera's cbSceneParam instance is jittered. Shadow views use different instances; they are identified by the viewport and matrix.
**Upload (confirmed in dumps 174226/174227):**
- A single 960-byte buffer is re-uploaded with `UpdateSubresource` at the start of each command list, once per view.
  - Shadow cascades: orthographic, `Proj[15]=1`.
  - A 512² square perspective view.
  - The main camera: perspective with a 16:9 aspect, used by all G-buffer, lighting, transparent, half-res and post lists.
- Matrices are stored as rows for `clip = M*v`. `VC_MatrixViewProj` is camera-relative, and `FC_MatrixInvViewProj` returns absolute world positions.
- Depth is standard Z (not reversed): near 0.08, `Proj[10]` = 1.00001.
- The jitter terms are `Proj[2]` / `Proj[6]` (0 in the game's data).
- Jitter math, implemented in `src/camera/scene_camera.cpp` (`[Jitter] Enabled`):
  - `row0 += jx*row3` and `row1 += jy*row3` on Proj and ViewProj;
  - `inv[r][3] -= jx*inv[r][0] + jy*inv[r][1]` on InvProj and InvViewProj.
  - Checked offline on dumped matrices: the shift is exact, depth is unchanged, and world reconstruction is unchanged.
- **Open:** command lists are recorded ahead of Present, so every list of one frame must get the same jitter phase.
  The list headers now show the capture frame each list was recorded in.

## DLAA insertion point

- **Input colour:** T5 (RGBA16F HDR). It is final after the half-res transparent composite (PS ACF66F57F6A70DCE, end of L54) and after the motion-blur velocity lists L55/L56.
- **Evaluate:** right before the first draw with `PS 393A947C35535471` (reads T5). The output is written back into T5.
- **Depth:** T0 (or its copy T2). **Motion vectors:** generated from depth plus matrices.
- **Resolution:** everything up to this point renders at 1920x1080, except the known half-res passes.

## DLAA implementation (`src/dlss/dlaa.cpp`)
- **Depth:** the scene depth is the screen-sized R32G8X24 texture the game clears with `ClearDepthStencilView`.
  It is copied each frame to our own texture, which has an `R32_FLOAT_X8X24_TYPELESS` SRV.
- **Motion vectors:** computed by `mv_from_depth.hlsl`.
  - `CurToPrev = inverse(InvViewProj_prev) * InvViewProj_cur`, computed in double on the CPU from unjittered matrices.
  - The shader removes the current jitter, reprojects, and outputs pixels (current → previous) in RG16F.
- **Evaluate:** runs once per frame on the immediate context before PS 393A.
  - Flags: `IsHDR | MVLowRes | AutoExposure`. Jitter = `JitterSign` × content offset in pixels.
  - The output is copied back into T5.
  - Reset triggers: the first frame, a frame gap, a size change, or the camera moving > 5 units in a frame.
- **State:** only the compute stage is saved and restored (shader, SRVs, UAVs, CBs, samplers).
  - `SwapDeviceContextState` + `ClearState` hung the GPU (`DXGI_ERROR_DEVICE_HUNG`) on driver 616.92.
  - It only hung without the debug layer, so it is not used.
- **Failure:** 30 failed evaluates in a row disable DLAA and the jitter (pass-through).

### Test results (`dlaa_test`, RTX 4070 SUPER, driver 616.92)
- **Motion vectors:** 0.0000 when static (jitter removal is exact); 0.9751 vs 0.9752 analytic when moving.
- **Flat grey scene:** stays 0.5005 after DLAA. Game VS / PS / CS bindings survive. No debug-layer messages.
- **Edge test:** mean error on edge pixels vs a supersampled reference, after 48 static frames:

  | Setup | Edge error |
  |---|---|
  | No AA | 0.228 |
  | DLAA, `JitterSign=1` | 0.192 |
  | DLAA, `JitterSign=-1` | 0.495 |

  So +1 (content offset in pixels, y down) is the correct sign.
