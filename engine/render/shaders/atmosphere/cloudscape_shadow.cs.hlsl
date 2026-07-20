#include "rhi_bindings.hlsli"
// Cloudscape ground shadows: attenuate the denoised sun shadow by the deck's
// optical depth along the sun ray, sampling the SAME textured density field
// the march renders (base noise + weather map + height profile, no erosion),
// so the shade on the ground tracks the formation that actually occludes the
// sun -- gaps stay lit, cores darken, and the pattern advects with the wind.
// Constants are kept in lockstep with cloudscape_march.cs.hlsl's cheap
// sampler; two taps through the shell are plenty at this softness.

struct ShadowPush {
  column_major float4x4 inv_view_proj;  // unjittered
  float3 sun_dir;  // travel direction
  float near_plane;
  float4 wind;   // xy blow direction (unit XZ), z speed m/s, w vertical skew m
  float4 shape;  // x bottom(m), y top(m), z density, w darkness
  float4 map;    // xy map offset (m), z map extent (m), w anvil
  float2 jitter;
  float strength;  // max darkening (0..1)
  float pad;
};
[[vk::binding(4, 0)]] ConstantBuffer<ShadowPush> pc : register(b4, space0);

[[vk::image_format("r8")]] [[vk::binding(0, 0)]] RWTexture2D<float> sun_shadow : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture3D<float4> base_noise : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState base_sampler : register(s2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D<float4> weather_map : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState weather_sampler : register(s3, space0);

static const float kBaseScale = 1.0 / 9600.0;  // matches cloudscape_march

float Remap(float v, float l0, float h0, float l1, float h1) {
  return l1 + (saturate((v - l0) / max(h0 - l0, 1e-5)) * (h1 - l1));
}

float HeightProfile(float h, float cloud_type, float anvil) {
  float stratus = Remap(h, 0.0, 0.06, 0.0, 1.0) * Remap(h, 0.18, 0.28, 1.0, 0.0);
  float strato = Remap(h, 0.02, 0.16, 0.0, 1.0) * Remap(h, 0.45, 0.62, 1.0, 0.0);
  float cumulus = Remap(h, 0.0, 0.12, 0.0, 1.0) * Remap(h, 0.68, 0.95, 1.0, 0.0);
  float profile = cloud_type < 0.5 ? lerp(stratus, strato, cloud_type * 2.0)
                                   : lerp(strato, cumulus, cloud_type * 2.0 - 1.0);
  float top = Remap(h, 0.6, 0.95, 0.0, 1.0);
  return lerp(profile, max(profile, top * 0.7), anvil * saturate(cloud_type * 2.0));
}

// The march's cheap density, in plain world space (flat-slab altitudes are
// indistinguishable from the shell at cast-shadow ranges).
float DensityCheap(float3 wp) {
  float h = (wp.y - pc.shape.x) / max(pc.shape.y - pc.shape.x, 1.0);
  if (h <= 0.0 || h >= 1.0) return 0.0;
  float4 weather = weather_map.SampleLevel(weather_sampler, (wp.xz - pc.map.xy) / pc.map.z, 0.0);
  float coverage = weather.r;
  if (coverage <= 0.005) return 0.0;
  float2 drift = pc.map.xy + pc.wind.xy * (pc.wind.w * h);
  float3 sp = (wp - float3(drift.x, 0.0, drift.y)) * kBaseScale;
  float4 nse = base_noise.SampleLevel(base_sampler, sp, 0.0);
  float worley_fbm = nse.g * 0.625 + nse.b * 0.25 + nse.a * 0.125;
  float base = Remap(nse.r, worley_fbm - 1.0, 1.0, 0.0, 1.0);
  base *= HeightProfile(h, weather.b, pc.map.w * weather.g);
  float d = Remap(base, 1.0 - coverage, 1.0, 0.0, 1.0) * coverage;
  return d * pc.shape.z;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  sun_shadow.GetDimensions(width, height);
  uint2 size = uint2(width, height);
  if (id.x >= size.x || id.y >= size.y) return;
  float depth = depth_map.Load(int3(id.xy, 0));
  if (depth <= 0.0) return;  // sky shades itself through the cloud march
  float existing = sun_shadow[id.xy];
  if (existing <= 0.01) return;

  float2 uv = (float2(id.xy) + 0.5) / float2(size);
  float2 ndc = uv * 2.0 - 1.0 - pc.jitter;
  float4 wh = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
  float3 world = wh.xyz / wh.w;
  float3 to_sun = normalize(-pc.sun_dir);
  if (to_sun.y <= 0.02) return;  // sun below the layer: no cast shadow

  float t0 = (pc.shape.x - world.y) / to_sun.y;
  float t1 = (pc.shape.y - world.y) / to_sun.y;
  if (t1 <= 0.0) return;  // ground above the layer top
  t0 = max(t0, 0.0);
  float3 p0 = world + to_sun * t0;
  float3 p1 = world + to_sun * t1;
  float span = (t1 - t0);
  // Two taps weighted by the path length; menace deepens the occlusion the
  // way it deepens the deck's own absorption.
  float d = DensityCheap(lerp(p0, p1, 0.16)) + DensityCheap(lerp(p0, p1, 0.65));
  float shade = exp(-d * span * 0.0016 * (1.0 + pc.shape.w * 1.5));
  float factor = lerp(1.0 - pc.strength, 1.0, shade);
  sun_shadow[id.xy] = existing * factor;
}
