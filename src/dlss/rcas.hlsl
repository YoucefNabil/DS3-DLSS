// Sharpening after DLAA: AMD FidelityFX RCAS (robust contrast-adaptive
// sharpening, from FSR 1). Each pixel is pushed away from its 4 neighbours by
// as much as it can go without leaving their min/max range, so edges get
// crisper without halos or ringing. RCAS expects 0..1 values; the HDR scene
// is compressed with a reversible Reinhard curve around it, so bright lights
// do not get dark outlines.
// Compiled with fxc (cs_5_0) into rcas.h at build time.

cbuffer Params : register(b0) {
  float g_Sharpness;  // 0 = none, 1 = RCAS at full strength
  uint2 g_Size;
};

Texture2D<float4> g_In : register(t0);  // DLAA output
RWTexture2D<float4> g_Out : register(u0);

// Largest amount the lobe may subtract: keeps the result stable (AMD's limit).
static const float kLimit = 0.25 - 1.0 / 16.0;

float3 Compress(float3 c) {
  c = max(c, 0.0);
  return c / (1.0 + max(c.r, max(c.g, c.b)));
}

float3 Expand(float3 c) {
  return c / max(1.0 - max(c.r, max(c.g, c.b)), 1.0 / 65504.0);
}

float3 Fetch(int2 p) { return Compress(g_In.Load(int3(clamp(p, int2(0, 0), int2(g_Size) - 1), 0)).rgb); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_Size)) return;
  const int2 p = int2(id.xy);
  const float4 centre = g_In.Load(int3(p, 0));
  //    b
  //  d e f
  //    h
  const float3 b = Fetch(p + int2(0, -1));
  const float3 d = Fetch(p + int2(-1, 0));
  const float3 e = Compress(centre.rgb);
  const float3 f = Fetch(p + int2(1, 0));
  const float3 h = Fetch(p + int2(0, 1));

  const float3 mn4 = min(min(b, d), min(f, h));
  const float3 mx4 = max(max(b, d), max(f, h));
  // How far the lobe can go before the result clips at the neighbourhood's
  // darkest (hit_min) or brightest (hit_max) value.
  const float3 hit_min = mn4 / max(4.0 * mx4, 1e-5);
  const float3 hit_max = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-5);
  const float3 lobe3 = max(-hit_min, hit_max);
  const float lobe = max(-kLimit, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0)) * g_Sharpness;

  const float3 sharp = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
  g_Out[id.xy] = float4(Expand(saturate(sharp)), centre.a);
}
