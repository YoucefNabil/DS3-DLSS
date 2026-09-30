// Full-screen triangle for the DebugView overlay (see dlaa.cpp, DrawDebugOverlay).
// Compiled with fxc (vs_5_0) into debug_blit_vs.h at build time.

float4 main(uint id : SV_VertexID) : SV_Position {
  const float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
