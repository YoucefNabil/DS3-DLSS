// Motion vectors for DLSS: per-object motion where the game's velocity pass
// covered the pixel (see object_motion.h), camera motion from depth elsewhere.
// Compiled with fxc (cs_5_0) into mv_from_depth.h at build time.

cbuffer Params : register(b0) {
  // Unjittered NDC of this frame (x, y, depth, 1) -> previous frame's clip space.
  row_major float4x4 g_CurToPrev;
  float2 g_JitterNdc;  // jitter this frame was rendered with, in NDC
  float2 g_Size;       // render size in pixels
  uint g_DebugView;    // 1: motion vectors, 2: reactive mask, 3: motion vector check
  uint g_UseObjectMotion;
  float g_ProjZ;       // FC_MatrixProj[10]: depth = g_ProjZ - near / view distance
  float g_Reactive;    // reactive mask strength, 0 = off
  float2 g_JitterPx;      // this frame's content offset, pixels (x right, y down)
  float2 g_PrevJitterPx;  // last frame's
};

Texture2D<float> g_Depth : register(t0);  // D32 depth (standard Z, 1 = far)
// Unjittered current minus previous NDC, surface depth, coverage (w = 1).
Texture2D<float4> g_ObjectMotion : register(t1);
// What blended effects added to each pixel (see reactive.h), and the final scene.
Texture2D<float4> g_Effects : register(t2);
Texture2D<float4> g_Scene : register(t3);
Texture2D<float4> g_PrevScene : register(t4);  // DebugView 3: last frame's scene
SamplerState g_Linear : register(s0);          // bilinear, clamp

RWTexture2D<float2> g_MotionVectors : register(u0);  // pixels, current -> previous
RWTexture2D<float> g_DepthOut : register(u1);        // depth copy DLSS can read
RWTexture2D<float4> g_Debug : register(u2);
RWTexture2D<float> g_Bias : register(u3);  // DLSS "bias current colour" mask
RWTexture2D<float> g_Object : register(u4);  // 1 = object motion (characters), for captures

float3 Compress(float3 c) {
  c = max(c, 0.0);
  return c / (1.0 + dot(c, float3(0.299, 0.587, 0.114)));
}

// DebugView 3: moves last frame's scene along the motion vectors and compares
// it with this frame's 3x3 colour range (what a temporal AA keeps). Correct
// vectors stay dark; red = last frame's colour does not belong here (wrong
// vector, or something just uncovered); blue = the vector leaves the screen.
float4 MotionCheck(int2 p, float2 mv) {
  const int2 last = int2(g_Size) - 1;
  float3 lo = 1e9, hi = -1e9;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      const float3 c = Compress(g_Scene.Load(int3(clamp(p + int2(x, y), int2(0, 0), last), 0)).rgb);
      lo = min(lo, c);
      hi = max(hi, c);
    }
  }
  const float3 cur = Compress(g_Scene.Load(int3(p, 0)).rgb);
  const float base = 0.4 * dot(cur, float3(0.299, 0.587, 0.114));
  // Pixel p shows the unjittered point p - jitter; last frame showed point q at q + its jitter.
  const float2 pos = float2(p) + 0.5 + mv - g_JitterPx + g_PrevJitterPx;
  if (any(pos < 0.0) || any(pos > g_Size)) return float4(base, base, 0.6, 1.0);
  const float3 h = Compress(g_PrevScene.SampleLevel(g_Linear, pos / g_Size, 0).rgb);
  const float3 outside = max(max(lo - h, h - hi), 0.0);
  const float error = saturate(max(outside.r, max(outside.g, outside.b)) * 5.0);
  return float4(base + error * (1.0 - base), base * (1.0 - error), base * (1.0 - error), 1.0);
}

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
  g_Object[id.xy] = object ? 1.0 : 0.0;
  if (g_DebugView == 1)
    g_Debug[id.xy] = float4(abs(mv) * 0.25, object ? 1.0 : (depth >= 1.0 ? 0.25 : 0.0), 1.0);
  else if (g_DebugView == 2)
    g_Debug[id.xy] = float4(bias, bias, bias, 1.0);
  else if (g_DebugView == 3)
    g_Debug[id.xy] = MotionCheck(int2(id.xy), mv);
}
