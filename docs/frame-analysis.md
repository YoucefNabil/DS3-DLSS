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
- **Failure:** 30 failed evaluates in a row disable DLAA and the jitter (pass-through),
  or switch to TAA with `Backend=Auto` (see below).

## TAA backend (`src/dlss/taa.hlsl`, `[DLSS] Backend`)
For GPUs without DLSS. It uses the same inputs as DLSS (jittered HDR scene, motion vectors, depth,
reactive mask) and runs at the same point, so the jitter, motion vectors and effects are shared.
- **Choice:** `Auto` tries NGX; if init fails (no RTX GPU, old driver, no `nvngx_dlss.dll`), feature
  creation fails, or 30 evaluates fail in a row, it switches to TAA for the session with a history reset.
  `DLSS` turns DLAA off instead, `TAA` never loads NGX.
- **Shader:** one compute pass, 8x8 threads.
  - Colours are compressed with luma Reinhard (`c / (1 + luma)`) and filtered in YCoCg.
  - The current frame is resampled at the unjittered pixel centre from its 3x3 neighbours
    (Blackman-Harris approximation `exp(-2.29 d²)`), so the image does not wobble with the jitter.
  - History is fetched at `pixel + mv` of the closest-depth neighbour (edges of moving objects keep
    their motion) with a 5-tap Catmull-Rom filter.
  - History is clipped towards the centre of a variance box (mean ± σ, intersected with min/max).
  - History of another surface is dropped (blend 1): where the depth stored with it, at any of the 2x2
    texels it is fetched from, differs from this pixel's view distance by more than 5-10%, or where its
    motion vector differs by more than 1 px + 25% of the speed. This is what removes the smear behind
    moving characters: on detailed surfaces the 3x3 colour box contains every colour, so the clip alone
    lets the old surface through.
  - Blend: 10% current frame (up to 20% at 16 px/frame of motion), raised to the reactive mask value;
    100% on reset or off-screen history.
- **Output:** written to our output texture, copied into the history, then into T5 (RCAS runs after
  the copy into the history, so sharpening does not accumulate).
- **Tests** (`dlaa_test`, 640x360):

  | Test | TAA | DLSS |
  |---|---|---|
  | Edge error after 48 static frames (no AA 0.228) | 0.151 | 0.192 |
  | Ghost (below): mean error inside the moving quad (no AA 0.065) | 0.036 | 0.066 |
  | Ghost: mean error just behind it (no AA 0.067) | 0.079 | 0.074 |

  In a folder without `nvngx_dlss.dll`, `Auto` logs `DLSS is not available, using TAA instead` and
  passes; `DLSS` logs `disabled for this session`. A flat, sharp-edged test scene flatters TAA:
  on real content it is softer than DLSS and can still flicker on sub-pixel detail.

### Character smear (2026-09-30)
In game, TAA blurred the moving character like bad motion blur (DLSS a little too).
- `dlaa_test ghost` reproduces it: a checkerboard quad moving over a checkerboard wall of the same two
  colours (~5 px cells, compared with an 8x8 supersampled reference).
  - First TAA: 0.036 inside, **0.124** behind (worse than no AA): the uncovered wall kept the quad's
    colours, which the colour clip accepts. The depth/motion rejection brings it to 0.079.
  - With `ObjectMotion=0` (wrong vectors on the quad) the inside error is 0.19 (TAA) / 0.16 (DLSS), and
    no history test can catch it: last frame's pixel really was the same surface at the same depth.
    A tighter colour box (0.75σ / 0.5σ) only reaches 0.170 / 0.150 and makes still edges worse
    (0.1545 / 0.2013 vs 0.1505), so it stays at 1σ.
- So wrong motion vectors on the character cannot be fixed in the TAA; they must be fixed at the source.
  `DebugView=3` shows where they are wrong (red): last frame's scene, moved along the vectors (with both
  frames' jitter removed), outside this frame's 3x3 colour range. `dlaa_test check`: 0.000 red on the quad
  with object motion, 0.119 without it.
- In game (`DebugView=3`, 2026-09-30): the world stays dark; the character turns red while the camera
  moves (worst when orbiting a standing character). Two read-only logs of the velocity draws followed
  (the code was removed afterwards; it is not in the build):
  - `cbMatrixWVP` (b0) in the velocity pass holds the **absolute** view-projection (camera translation
    included) of this frame, and `VC_PrevMatrixWVP` exactly last frame's (max relative difference 0.0000
    in all 30 logged frames, still and orbiting). `VC_NowToPrevView` is the view-space camera motion.
    So the pass includes camera motion.
  - Bone palettes (`cbObjMatrix` b5 / `cbPrevObjMatrix` b6) are 1536-byte (also 2304 / 4128) buffers
    written with UpdateSubresource, mostly on deferred contexts. Current bones are indexed with the
    `cbInstanceData[5].x` offset, previous bones without it (the game uploads matching chunks).
  - For the standing player while orbiting, each bone's game motion equals the camera-only motion to
    within 0.2 px (the rest is idle animation); static objects to 0.1 px. The velocity data is correct.
  - The velocity lists are sometimes recorded before the camera upload of the same frame (worker
    threads), but always within the same Present interval (frame dumps: every list `recorded frame
    0..0`), so the jitter keyed to the frame counter is right. Using the latest camera's jitter instead
    would be wrong in exactly that case (tried, reverted).
  - Conclusion: the red on the character in motion is colour that really changes between frames
    (view-dependent specular, shadow-map and AO changes while the camera turns), not wrong vectors.
    The temporal AA correctly drops history there, which is the remaining softness in motion.

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

## Object motion (`src/dlss/object_motion.*`)
DS3's motion blur draws moving objects into a velocity buffer. These are deferred lists L55/L56, into a 960x540 RGBA8 target with DS T60.
- **Vertex shaders:** `C48361A10D38D8D5`, `96632548D75B4C2F`, `F54DC44863BE505A`, `3E098128A8A5C75A`.
  - Inputs: `cbMatrixWVP` (b0: `VC_MatrixWVP`, `VC_PrevMatrixWVP`, unjittered) and `cbPrevObjMatrix` (b6), plus skinning with current and previous bones.
  - Outputs: TEXCOORD0 = current clip position, TEXCOORD1 = previous clip position.
- **Pixel shaders:** `940604D2B2BCB300` and its alpha-tested variants `CFEAC2188D902F4E`, `74CCBD38E2950ECA`, `F3EAC67A647852DD`.
  - Output: `xy = sat((cur_ndc - prev_ndc) * cb7[0].xy * 0.5 + 0.5)`, `z = w / (w + 10)`.
- **Dilation:** PS `33B0AAA91300023E` dilates the result.

The proxy recognises these VS (by hash, or by the strings `VC_PrevMatrixWVP` + `cbPrevObjMatrix`).
- After each draw made with one of them, it repeats the draw once with its own PS (`object_motion.hlsl`).
  - The target is a full-res RGBA16F texture.
  - The PS does a manual depth test against the scene depth (3% view distance), and outputs `cur_ndc - prev_ndc`.
  - The velocity VS are unjittered, so the repeat's viewport is offset by the frame's jitter
    (`TopLeftX/Y = jitter in pixels`). The rasterised silhouette then matches the jittered scene exactly.
    The motion values come from the VS matrices and are not affected.
- **State:** only RTVs/DSV, PS, PS SRV t20, PS CB b13, blend, viewports and scissors are swapped, and they are restored right after.
- **Merge:** `mv_from_depth.hlsl` uses object motion where the pixel is covered, and camera reprojection everywhere else.
  - Before the viewport jitter, silhouettes could be off by up to a pixel. The CS then also accepted a covered
    3x3 neighbour at the same depth. That workaround guessed at the edges, and it was removed.
- **Motion Blur must be on** in the game settings: with it off, the velocity lists are not recorded at all.
  - With `HideMotionBlur=1` the blur's own draws are skipped: two DrawIndexed calls with PS `E484BFEBC00C5CF7` (scene → temp → scene).
  - The tile pass (PS `D55E5F7C1E2C4722`) still runs; its result is unused.
- The game creates each velocity VS many times (64+ objects), so the shader pointers are kept in a large lock-free set.
- **Test** (`dlaa_test object`): 0.9725 vs 0.9743 analytic on the moving quad; 0 when still; background unaffected.
  - Every pixel near the quad's edges must be flagged exactly where the jittered scene covers it (frames 6–11).
  - Without the viewport offset this fails, e.g. on frame 6 with jitter +0.375.

## Reactive mask (transparent effects)
Effects (bonfire-warp glow, spells, fire, particles) have no depth or motion vectors, so DLSS
reprojects its history under them and flickers or smears whatever they cover.
- **Detection:** a draw is an effect when RT0 is the HDR scene (the texture DLAA runs on), RT1 is also
  bound (DS3's forward/transparent passes use two targets; deferred lighting writes the scene alone),
  RT0 blending is on, and the depth-stencil state writes neither depth nor stencil.
- **Redraw:** each such draw is repeated once into a black full-res RGBA16F target, with the game's own
  shaders, blend state and DSV. Only the RTVs are swapped (`OMSetRenderTargetsAndUnorderedAccessViews`
  with `D3D11_KEEP_UNORDERED_ACCESS_VIEWS`) and restored right after. The result is what the effects
  added to each pixel.
- **Mask:** `mv_from_depth.hlsl` computes `saturate(max(fx.rgb) / max(scene.rgb) * Reactive)` into an
  R8 texture, passed to DLSS as `pInBiasCurrentColorMask`. The effect target is cleared after each evaluate.
  History resets set the strength to 0 for that frame.
- **Test** (`dlaa_test effect`, DebugView=2): 0.498 on an additive 0.5-over-0.5 quad (expected 0.5),
  0 on an opaque quad drawn into the same targets, 0 elsewhere.

### Dither fades (bonfire warp flicker)
While the player warps, DS3 stops drawing them in the G-buffer pass (6 targets) and draws them in the
forward pass (scene + T24) instead, faded with a screen-space dither:
- Depth pre-pass: VS `FCC46DD7901DED1C` + PS `355B4107029B692E` (discards by `g_DitherTexture` t38,
  sampled at pixel/4, against `ditherFadeAlpha` in cbInstanceData), and VS `11503580C516F154` with no PS.
- Shading: VS `3E829D3129D80CBF` + PS `6E984A7E0989E6E7` / `9149E095B4B17A5F` (the latter dithers too).
- The velocity pass still draws the whole silhouette (VS `C48361A10D38D8D5`).

A first attempt masked these fading draws in the reactive mask (`ReactiveFade`). The character went fully
white in DebugView=2 but still flickered, so it was removed. The dither itself is not the problem.
The cause is that the two passes use different camera matrices:
- The pre-pass VSs (`11503580…`, `FCC46DD7…` and 559 other forward VSs) transform with
  `cbMatrixWVP` (b0, 192 bytes) in its per-view layout: row_major `VC_View_MatrixCamera` (0, row 3 =
  camera position, subtracted first), `VC_View_MatrixViewProj` (64, camera-relative, same w-row
  convention as cbSceneParam) and `VC_ViewFrustumPlanes[4]` (128).
- The shading VS uses `cbSceneParam.VC_MatrixViewProj`, which was the only one jittered. Its depth differed from
  the pre-pass by a different sub-pixel amount each frame, so its depth test dropped different pixels each frame.

The main camera's `cbMatrixWVP` is therefore jittered too (`camera::JitterViewCb`), via UpdateSubresource or
Map(WRITE_DISCARD)/Unmap. It is recognised by content: a rigid camera matrix (column 3 = 0,0,0,1)
and a perspective ViewProj with a unit forward w-row, w translation 0, and the screen aspect.
The velocity pass's buffer of the same size (object WVP / PrevWVP / NowToPrevView) fails these
tests and stays unjittered, as the motion vectors need. The log line "jitter: N cbMatrixWVP uploads jittered last
frame" shows it working. Confirmed in game: the warp fade-out is smooth.

## Mip bias (`src/dlss/mip_bias.*`)
DLSS rebuilds detail from many jittered frames, so textures can come from a sharper mip than the game
picks. NVIDIA recommends a LOD bias of `log2(render / output) - 1`, which is -1 for DLAA.
- **Twins:** the `CreateSamplerState` hook creates a second sampler with `MipLODBias += MipBias` next to
  each one that uses mips. Skipped: comparison and min/max filters (shadow maps), `MIN_MAG_MIP_POINT`,
  and `MaxLOD <= 0` or `MaxLOD <= MinLOD`. The game only ever sees its own sampler. The twin is kept in a
  map keyed by the original. A private-data `IUnknown` on the original removes that map entry when the
  runtime destroys the sampler, so a reused address never picks up a stale twin.
- **Swap:** `PSSetSamplers` binds the twin instead of the original from Present until DLAA runs (the first
  post pass). Post-processing and UI draws get the originals. The End key (`MipBiasKey`) and the DLAA
  toggle switch it off without recreating anything.
- **Test:** every `dlaa_test` mode binds an anisotropic, a comparison and a point sampler before and
  after the DLAA pass and reads them back. It expects the configured bias on the anisotropic one before
  DLAA only, and 0 everywhere else. With `MipBias=0` the check fails, as it should.

## Sharpening (`src/dlss/rcas.hlsl`)
NGX's sharpness parameter is ignored since DLSS 2.5.1, so `[DLSS] Sharpness` runs AMD FidelityFX RCAS
(FSR 1) as a compute pass on the DLAA output. The result goes into a separate RGBA16F texture and is copied
back into the scene instead of the raw output. RCAS moves each pixel away from its 4 neighbours, limited so
that it stays inside their min/max. It needs 0..1 input, so the HDR colour goes through a reversible
Reinhard curve, `c / (1 + max(c))`, and is expanded again afterwards. The strength scales AMD's lobe
linearly (1 = RCAS at 0 stops). The cost is one full-screen pass plus a copy. Test (`dlaa_test edge`):
edge-pixel error vs the supersampled reference 0.1917 / 0.1922 / 0.1926 / 0.1933 at 0 / 0.3 / 0.6 / 1
(sharper than the exact coverage, as expected). The flat grey scene stays 0.50.

## Camera matrix audit (`tools/cb_scan.cpp`, `src/camera/scene_camera.cpp`)
The jitter first went only into cbSceneParam (b8) and the per-object cbMatrixWVP. The warp flicker showed
that any shader holding its *own* copy of the camera matrix renders unjittered, so its output does not
line up with the jittered scene. `cb_scan <DS3_DLSS_shaders>` reflects every dumped shader and lists each
cbuffer variable (name, offset, size, type, used). 36 cbuffer layouts carry float4x4/4x3 matrices. After
dropping object/bone/world-only buffers, these hold a main-camera copy:

| Size | Buffer (examples) | Offsets | Seen in frame dumps |
|---|---|---|---|
| 64 | cbMatrixWVP (64), cbwvpmatrix, cbMatrixCamera | 0 | cbwvpmatrix: VS 783C19FD depth-only |
| 80 | cbLightAccVtxParam, cbParam | 0, 16 | no |
| 112 | cbPerFrame | 0 | no |
| 224 | fcbVolumeRenderParam | 96, 160 | no |
| 288 | cbSSAOParam2 | 0 | SSAO runs at 960x540 |
| 320 | cbLightAccParam `g_InvProjMatrix`, cbPerScene | 0, 64, 208 | tiled lighting CS E282BB9D, light PS F89A64C9/ECF86353 |
| 336 | cbPerScene (shadow collect) | 0 | PS 6FE9AFB4 & co. rebuild view pos with InvProj |
| 416 | cbDecalParam | 0, 64 | no |
| 448 | VSCommonBuffer, gcbSceneParam | 0, 128, 192, 208 | VSCommonBuffer WVP: VS 9A739EDD/F48B81D9 |
| 576 | cbAOFields | 192, 384 | no |
| 640 | cbWave | 64, 192, 256, 384 | no |
| 864 | cbSceneParam (864) | 80, 144, 208, 800 | no |
| 1104 | ccbSceneParam | 928, 992 | no |

All of them were unjittered, so deferred lighting, specular and shadow edges were computed at the
un-offset pixel centre: temporally they did not move with the geometry, and DLAA could not anti-alias them
(and could smear them slightly). Nothing with a camera matrix draws after the DLAA point.

**Tried and reverted (2026-09-30).** A generic fix jittered every matrix in these buffers that was a
forward or inverse copy of the main camera's ViewProj (this frame's or the previous frame's), in either
orientation. In game it matched a lot per frame: 64-byte WVPs x185, gcbSceneParam @0/@128, cbAOFields @384,
cbLightAccParam, SSAO, decals. The aliasing came back in full. The likely cause is that the net was too wide.
Buffers used for reprojection or velocity (the game's motion-blur velocity VS that ObjectMotion redraws,
previous-frame matrices matched against the previous reference) got the current jitter too. So the motion
vectors carried the jitter delta, and DLAA cancelled its own jitter. Any retry has to be per-buffer, one at
a time (for example lighting InvProj only), checked in game with A/B each time, and must never touch
matrices read by the velocity pass or previous-frame matrices. `cb_scan` stays as the tool for that.

## Half-resolution camera copy (fixed 2026-09-30)
Some areas upload a second cbSceneParam for a 960x540 pass, with the same camera, before the full-resolution
one. It passed the main-camera test (perspective, screen aspect), so on those frames it became the camera
snapshot. DLAA then saw "scene colour and camera screen size differ" and skipped: the jitter stayed on and
the image shook, and it was aliased (2 of 60 frames evaluated in the log). The per-view jitter also used
960x540 as the screen size. Now only a cbSceneParam whose screen size equals the swap chain's is the main
camera. Smaller copies get the same NDC jitter as the full-resolution scene and are logged once
("jitter: 960x540 copy of the main camera …"). Test: every `dlaa_test` mode uploads a half-size copy first.

## Lighting camera jitter (tried and removed 2026-09-30)
A narrow retry of the audit: only cbLightAccParam (320 B) and cbPerScene (336 B) InvProj at offset 0,
replaced by the jittered InvProj when it exactly matched this frame's unjittered one. In game the bonfires'
bright lights flickered, so it was removed at the user's request.
