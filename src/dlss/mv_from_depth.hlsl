// Motion vectors for DLSS: per-object motion where the game's velocity pass
// covered the pixel (see object_motion.h), camera motion from depth elsewhere.
// Compiled with fxc (cs_5_0) into mv_from_depth.h at build time.

cbuffer Params : register(b0) {
  // Unjittered NDC of this frame (x, y, depth, 1) -> previous frame's clip space.
  row_major float4x4 g_CurToPrev;
  float2 g_JitterNdc;  // jitter this frame was rendered with, in NDC
  float2 g_Size;       // render size in pixels
  uint g_DebugView;    // 1: write a motion-vector visualization to g_Debug
  uint g_UseObjectMotion;
  float g_ProjZ;       // FC_MatrixProj[10]: depth = g_ProjZ - near / view distance
  float g_Reactive;    // reactive mask strength, 0 = off
};

Texture2D<float> g_Depth : register(t0);  // D32 depth (standard Z, 1 = far)
// Unjittered current minus previous NDC, surface depth, coverage (w = 1).
Texture2D<float4> g_ObjectMotion : register(t1);
// What blended effects added to each pixel (see reactive.h), and the final scene.
Texture2D<float4> g_Effects : register(t2);
Texture2D<float4> g_Scene : register(t3);

RWTexture2D<float2> g_MotionVectors : register(u0);  // pixels, current -> previous
RWTexture2D<float> g_DepthOut : register(u1);        // depth copy DLSS can read
RWTexture2D<float4> g_Debug : register(u2);
RWTexture2D<float> g_Bias : register(u3);  // DLSS "bias current colour" mask

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= (uint2)g_Size)) return;

  const float depth = g_Depth.Load(int3(id.xy, 0));
  const float2 uv = (id.xy + 0.5) / g_Size;
  // The jittered frame shows the surface point of NDC p at p + jitter.
  const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - g_JitterNdc;
  const float4 prev = mul(g_CurToPrev, float4(ndc, depth, 1.0));

  float2 mv = 0;
  if (prev.w > 1e-6) mv = (prev.xy / prev.w - ndc) * float2(0.5, -0.5) * g_Size;

  // Object motion where the velocity redraw covered this pixel (it is drawn
  // with the same jitter as the scene, so the silhouettes match).
  bool object = false;
  if (g_UseObjectMotion != 0) {
    const float4 om = g_ObjectMotion.Load(int3(id.xy, 0));
    object = om.w > 0.0;
    if (object) mv = -om.xy * float2(0.5, -0.5) * g_Size;
  }

  // Share of the pixel's brightness that comes from transparent effects.
  float bias = 0.0;
  if (g_Reactive > 0.0) {
    const float3 fx = max(g_Effects.Load(int3(id.xy, 0)).rgb, 0.0);
    const float3 scene = max(g_Scene.Load(int3(id.xy, 0)).rgb, 0.0);
    const float fx_max = max(fx.r, max(fx.g, fx.b));
    const float scene_max = max(scene.r, max(scene.g, scene.b));
    bias = saturate(fx_max / max(scene_max, 1e-4) * g_Reactive);
  }

  g_MotionVectors[id.xy] = mv;
  g_DepthOut[id.xy] = depth;
  g_Bias[id.xy] = bias;
  if (g_DebugView == 1)
    g_Debug[id.xy] = float4(abs(mv) * 0.25, object ? 1.0 : (depth >= 1.0 ? 0.25 : 0.0), 1.0);
  else if (g_DebugView == 2)
    g_Debug[id.xy] = float4(bias, bias, bias, 1.0);
}
