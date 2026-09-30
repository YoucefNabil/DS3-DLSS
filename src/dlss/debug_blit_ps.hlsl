// Draws the DebugView image over the finished frame (after tone
// mapping, auto exposure and UI), so its colours mean what they say.
// Compiled with fxc (ps_5_0) into debug_blit_ps.h at build time.

Texture2D<float4> g_Debug : register(t0);

float4 main(float4 pos : SV_Position) : SV_Target {
  return float4(saturate(g_Debug.Load(int3(pos.xy, 0)).rgb), 1.0);
}
