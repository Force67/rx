#include "rhi_bindings.hlsli"
// Motion blur, stage 2: velocity-weighted gather along the tile
// neighborhood's dominant velocity (McGuire 2012, simplified). Runs on the
// post-AA color at output resolution; per-sample weighting by the sample's
// own velocity keeps sharp foregrounds from smearing over static backgrounds
// and lets moving objects streak over both.
struct MotionBlurPush {
  uint2 size;        // output resolution
  float2 inv_size;
  uint2 tile_count;
  float2 vel_scale;  // uv velocity -> blur vector (matches tilemax)
  float2 max_blur;
  uint samples;
  uint frame_index;
  float2 debug_vel;  // nonzero: override every velocity (static-camera testing)
};
PUSH_CONSTANTS(MotionBlurPush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D color : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState color_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float2> motion : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState motion_sampler : register(s2, space0);
[[vk::binding(3, 0)]] Texture2D<float2> tile_max : register(t3, space0);

float Ign(float2 pixel, uint frame) {
  pixel += float(frame & 63u) * 5.588238;
  return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  float2 uv = (float2(id.xy) + 0.5) * pc.inv_size;
  float4 center = color.SampleLevel(color_sampler, uv, 0.0);

  // Dominant velocity from the 3x3 tile neighborhood.
  int2 tile = int2(id.xy / 16u);
  float2 vmax = 0.0.xx;
  float vmax_mag = 0.0;
  [unroll]
  for (int ty = -1; ty <= 1; ++ty) {
    [unroll]
    for (int tx = -1; tx <= 1; ++tx) {
      int2 t = clamp(tile + int2(tx, ty), int2(0, 0), int2(pc.tile_count) - 1);
      float2 v = tile_max.Load(int3(t, 0));
      float m = dot(v, v);
      if (m > vmax_mag) {
        vmax_mag = m;
        vmax = v;
      }
    }
  }

  // Under half a pixel of blur: nothing to do.
  float px_mag = length(vmax * float2(pc.size));
  if (px_mag < 0.5) {
    out_color[id.xy] = center;
    return;
  }

  bool dbg = any(pc.debug_vel != 0.0);
  float2 own_v = dbg ? pc.debug_vel
                     : clamp(-motion.SampleLevel(motion_sampler, uv, 0.0) * pc.vel_scale,
                             -pc.max_blur, pc.max_blur);
  float own_mag = max(length(own_v * float2(pc.size)), 0.5);

  float jitter = Ign(float2(id.xy), pc.frame_index) - 0.5;
  float3 sum = center.rgb;
  float weight_sum = 1.0;
  uint n = max(pc.samples, 2u);
  for (uint i = 0; i < n; ++i) {
    float t = ((float(i) + 0.5 + jitter) / float(n)) - 0.5;  // -0.5 .. 0.5
    float2 suv = uv + vmax * t;
    if (any(suv < 0.0) || any(suv > 1.0)) continue;
    float2 sv = dbg ? pc.debug_vel
                    : clamp(-motion.SampleLevel(motion_sampler, suv, 0.0) * pc.vel_scale,
                            -pc.max_blur, pc.max_blur);
    float s_mag = length(sv * float2(pc.size));
    float dist_px = abs(t) * px_mag;
    // A sample contributes if IT moved far enough to cross this pixel (it
    // smears here) or if this pixel itself is moving (background gathers).
    float w = saturate(1.0 - dist_px / max(max(s_mag, own_mag), 1e-3));
    sum += color.SampleLevel(color_sampler, suv, 0.0).rgb * w;
    weight_sum += w;
  }
  out_color[id.xy] = float4(sum / weight_sum, center.a);
}
