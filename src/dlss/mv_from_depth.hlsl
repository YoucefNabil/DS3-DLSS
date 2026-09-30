// Camera motion vectors for DLSS, reconstructed from the scene depth.
// Compiled with fxc (cs_5_0) into mv_from_depth.h at build time.

cbuffer Params : register(b0) {
  // Unjittered NDC of this frame (x, y, depth, 1) -> previous frame's clip space.
  row_major float4x4 g_CurToPrev;
  float2 g_JitterNdc;  // jitter this frame was rendered with, in NDC
  float2 g_Size;       // render size in pixels
  uint g_DebugView;    // 1: write a motion-vector visualization to g_Debug
  uint3 g_Pad;
};

Texture2D<float> g_Depth : register(t0);  // D32 depth (standard Z, 1 = far)

RWTexture2D<float2> g_MotionVectors : register(u0);  // pixels, current -> previous
RWTexture2D<float> g_DepthOut : register(u1);        // depth copy DLSS can read
RWTexture2D<float4> g_Debug : register(u2);

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

  g_MotionVectors[id.xy] = mv;
  g_DepthOut[id.xy] = depth;
  if (g_DebugView == 1) g_Debug[id.xy] = float4(abs(mv) * 0.25, depth >= 1.0 ? 0.25 : 0.0, 1.0);
}
