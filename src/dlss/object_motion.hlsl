// Replaces the game's motion-blur velocity pixel shaders (940604D2B2BCB300 and
// its alpha-tested variants) when their draws are repeated at full resolution.
// The game's vertex shaders output the current and previous clip position of
// each vertex (object and bone motion included, no jitter); this keeps them in
// float instead of the game's half-res 8-bit encoding.
// Compiled with fxc (ps_5_0) into object_motion.h at build time.

cbuffer Params : register(b13) {
  float g_ProjZ;  // FC_MatrixProj[10]: depth d at view distance z is g_ProjZ - near / z
  float3 g_Pad;
};

Texture2D<float> g_SceneDepth : register(t20);  // the game's depth buffer (standard Z)

float4 main(float4 pos : SV_Position, float4 cur : TEXCOORD0, float4 prev : TEXCOORD1)
    : SV_Target {
  // Only the visible surface counts: compare view distances. The viewport
  // carries the jitter, so this is the scene's own surface up to rounding; the
  // tolerance covers small differences between the game's two vertex shaders.
  const float scene = g_SceneDepth.Load(int3(pos.xy, 0));
  const float scene_inv_z = g_ProjZ - scene;  // proportional to 1 / view distance
  const float inv_z = g_ProjZ - pos.z;
  if (scene_inv_z <= 0.0 || abs(inv_z / scene_inv_z - 1.0) > 0.03) discard;
  if (cur.w <= 1e-6 || prev.w <= 1e-6) discard;
  // Current minus previous NDC, the surface depth, and w marking the pixel as covered.
  return float4(cur.xy / cur.w - prev.xy / prev.w, pos.z, 1.0);
}
