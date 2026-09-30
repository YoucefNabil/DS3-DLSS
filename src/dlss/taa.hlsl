// Temporal anti-aliasing, used when DLSS is not available ([DLSS] Backend).
// It takes the same inputs as DLSS: the jittered HDR scene, motion vectors
// (pixels, current -> previous, see mv_from_depth.hlsl), depth and the
// reactive mask.
//  1. The current frame is resampled at the unjittered pixel centre from its
//     3x3 neighbours (Blackman-Harris weights), so the image does not wobble
//     with the jitter.
//  2. Last frame's result is fetched where this pixel was (motion vector of
//     the closest neighbour, so edges of moving objects keep their motion),
//     with a Catmull-Rom filter to avoid blurring it again every frame.
//  3. History that belongs to another surface is dropped: where the depth
//     stored with it differs from this pixel's (something just uncovered, or a
//     motion vector pointing at the wrong object), or where its motion differs
//     from this pixel's. The colour clip alone cannot catch this on detailed
//     surfaces, where the neighbourhood already contains every colour.
//  4. The remaining history is clipped to the colour range of the 3x3
//     neighbourhood (mean +- standard deviation, YCoCg).
//  5. Both are blended; fast motion and effects in the reactive mask use more
//     of the current frame.
// Colours are compressed with a luma Reinhard curve while filtering, so a
// single very bright pixel does not dominate its neighbours (less flicker).
// Compiled with fxc (cs_5_0) into taa.h at build time.

cbuffer Params : register(b0) {
  float2 g_Jitter;  // this frame's sample offset, pixels (x right, y down)
  uint2 g_Size;
  float g_Reset;     // 1 = no usable history (first frame, camera cut)
  float g_UseBias;   // 1 = g_Bias holds the reactive mask
  float g_Blend;     // share of the current frame in the result
  float g_ProjZ;     // FC_MatrixProj[10]: depth = g_ProjZ - near / view distance
};

Texture2D<float4> g_Color : register(t0);    // jittered HDR scene
Texture2D<float4> g_History : register(t1);  // last frame's result
Texture2D<float2> g_Motion : register(t2);
Texture2D<float> g_Depth : register(t3);     // standard Z, 1 = far
Texture2D<float> g_Bias : register(t4);      // 1 = use the current frame only
Texture2D<float2> g_PrevMotion : register(t5);  // last frame's g_Motion
Texture2D<float> g_PrevDepth : register(t6);    // last frame's g_Depth
SamplerState g_Linear : register(s0);        // bilinear, clamp
RWTexture2D<float4> g_Out : register(u0);

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 Compress(float3 c) {
  c = max(c, 0.0);
  return c / (1.0 + Luma(c));
}

float3 Expand(float3 c) { return c / max(1.0 - Luma(c), 1.0 / 65504.0); }

float3 ToYCoCg(float3 c) {
  return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)),
                dot(c, float3(-0.25, 0.5, -0.25)));
}

float3 FromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Proportional to the view distance (the unknown near plane cancels in ratios).
float Distance(float depth) { return 1.0 / max(g_ProjZ - depth, 1e-7); }

// Tuning, in pixels and fractions of the view distance.
static const float kDepthTolerance = 0.05;  // the same surface moves < 5% closer per frame
static const float kDepthRamp = 0.05;       // fully rejected at 10%
static const float kMotionTolerance = 1.0;  // px, plus 25% of the speed (camera acceleration)
static const float kFastBlend = 0.2;        // current-frame share at kFastSpeed and above
static const float kFastSpeed = 16.0;       // px per frame
static const float kBoxSigma = 1.0;         // colour box half-size, standard deviations

// Catmull-Rom history fetch in 5 bilinear taps (corners dropped); pos in pixels.
float3 SampleHistory(float2 pos) {
  const float2 size = float2(g_Size);
  const float2 t1 = floor(pos - 0.5) + 0.5;
  const float2 f = pos - t1;
  const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
  const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
  const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
  const float2 w3 = f * f * (-0.5 + 0.5 * f);
  const float2 w12 = w1 + w2;
  const float2 t0 = (t1 - 1.0) / size;
  const float2 t3 = (t1 + 2.0) / size;
  const float2 t12 = (t1 + w2 / w12) / size;
  float3 c = 0.0;
  float w = 0.0;
  float k;
  k = w12.x * w0.y;  c += g_History.SampleLevel(g_Linear, float2(t12.x, t0.y), 0).rgb * k;  w += k;
  k = w0.x * w12.y;  c += g_History.SampleLevel(g_Linear, float2(t0.x, t12.y), 0).rgb * k;  w += k;
  k = w12.x * w12.y; c += g_History.SampleLevel(g_Linear, t12, 0).rgb * k;                  w += k;
  k = w3.x * w12.y;  c += g_History.SampleLevel(g_Linear, float2(t3.x, t12.y), 0).rgb * k;  w += k;
  k = w12.x * w3.y;  c += g_History.SampleLevel(g_Linear, float2(t12.x, t3.y), 0).rgb * k;  w += k;
  return max(c / w, 0.0);
}

// Moves h towards the box centre until it is inside the box.
float3 ClipToBox(float3 h, float3 lo, float3 hi) {
  const float3 center = 0.5 * (hi + lo);
  const float3 extents = 0.5 * (hi - lo) + 1e-6;
  const float3 v = h - center;
  const float3 a = abs(v / extents);
  const float m = max(a.x, max(a.y, a.z));
  return m > 1.0 ? center + v / m : h;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= g_Size)) return;
  const int2 p = int2(id.xy);
  const int2 last = int2(g_Size) - 1;

  float3 filtered = 0.0, m1 = 0.0, m2 = 0.0;
  float3 lo = 1e9, hi = -1e9;
  float weight = 0.0;
  float closest = 2.0;
  int2 closest_p = p;
  float alpha = 1.0;
  [unroll] for (int y = -1; y <= 1; ++y) {
    [unroll] for (int x = -1; x <= 1; ++x) {
      const int2 q = clamp(p + int2(x, y), int2(0, 0), last);
      const float4 raw = g_Color.Load(int3(q, 0));
      const float3 c = ToYCoCg(Compress(raw.rgb));
      if (x == 0 && y == 0) alpha = raw.a;
      // This sample shows the unjittered point (x, y) - jitter away from us.
      const float2 d = float2(x, y) - g_Jitter;
      const float w = exp(-2.29 * dot(d, d));
      filtered += c * w;
      weight += w;
      m1 += c;
      m2 += c * c;
      lo = min(lo, c);
      hi = max(hi, c);
      const float z = g_Depth.Load(int3(q, 0));
      if (z < closest) {
        closest = z;
        closest_p = q;
      }
    }
  }
  filtered /= weight;

  const float2 motion = g_Motion.Load(int3(closest_p, 0));
  const float2 prev = float2(p) + 0.5 + motion;
  const bool outside = any(prev < 0.0) || any(prev > float2(g_Size));
  const float speed = length(motion);
  float blend = lerp(g_Blend, max(g_Blend, kFastBlend), saturate(speed / kFastSpeed));
  if (g_UseBias != 0.0) blend = max(blend, g_Bias.Load(int3(p, 0)));
  if (g_Reset != 0.0 || outside) blend = 1.0;

  if (blend < 1.0) {
    // The 2x2 texels the history is fetched from: keep it if any of them is
    // the same surface (depth within tolerance) moving the same way.
    const int2 base = clamp(int2(floor(prev - 0.5)), int2(0, 0), last - 1);
    const float dist = Distance(closest);
    float reject = 1.0;
    [unroll] for (int i = 0; i < 4; ++i) {
      const int2 q = base + int2(i & 1, i >> 1);
      const float depth_diff = abs(Distance(g_PrevDepth.Load(int3(q, 0))) / dist - 1.0);
      const float motion_diff = length(g_PrevMotion.Load(int3(q, 0)) - motion);
      const float r = max(saturate((depth_diff - kDepthTolerance) / kDepthRamp),
                          saturate(motion_diff / (kMotionTolerance + 0.25 * speed) - 1.0));
      reject = min(reject, r);
    }
    blend = max(blend, reject);
  }

  float3 result = filtered;
  if (blend < 1.0) {
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 box_lo = max(lo, mean - kBoxSigma * sigma);
    const float3 box_hi = min(hi, mean + kBoxSigma * sigma);
    float3 history = ToYCoCg(Compress(SampleHistory(prev)));
    history = ClipToBox(history, box_lo, box_hi);
    result = lerp(history, filtered, blend);
  }
  g_Out[p] = float4(Expand(max(FromYCoCg(result), 0.0)), alpha);
}
