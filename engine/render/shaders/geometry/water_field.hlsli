// Sampling side of the persistent water foam/ripple field (see water_field.cc).
// Two nested camera-following rings live in env slots 30/31, their origins and
// extents in the params CB at slot 32. water.ps samples them by world XZ for
// advected foam and near-camera ripple detail when kFrameFlagWaterField is set.
#ifndef RX_WATER_FIELD_HLSLI_
#define RX_WATER_FIELD_HLSLI_

struct WaterFieldParams {
  float4 ring[2];  // origin.xz, half_extent (m), texel_world (m)
};
[[vk::binding(32, 2)]] ConstantBuffer<WaterFieldParams> water_field : register(b32, space2);

[[vk::combinedImageSampler]] [[vk::binding(30, 2)]] Texture2D<float4> water_field_ring0 : register(t30, space2);
[[vk::combinedImageSampler]] [[vk::binding(30, 2)]] SamplerState water_field_ring0_sampler : register(s30, space2);
[[vk::combinedImageSampler]] [[vk::binding(31, 2)]] Texture2D<float4> water_field_ring1 : register(t31, space2);
[[vk::combinedImageSampler]] [[vk::binding(31, 2)]] SamplerState water_field_ring1_sampler : register(s31, space2);

float2 WaterFieldUv(uint r, float2 world) {
  return (world - water_field.ring[r].xy) / (2.0 * water_field.ring[r].z) + 0.5;
}

// 1 well inside the ring, fading to 0 across the outermost ~5% so the transition
// between rings (and to open water past ring 1) is seamless.
float WaterFieldInside(float2 uv) {
  float m = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
  return saturate(m / 0.05);
}

// Blended field sample: R ripple height, G ripple velocity, B foam density,
// A foam age. Ring 0 (near, detailed) takes priority; ring 1 fills the annulus;
// zero past ring 1.
float4 SampleWaterField(float2 world) {
  float2 uv0 = WaterFieldUv(0, world);
  float2 uv1 = WaterFieldUv(1, world);
  float4 s0 = water_field_ring0.SampleLevel(water_field_ring0_sampler, saturate(uv0), 0.0);
  float4 s1 = water_field_ring1.SampleLevel(water_field_ring1_sampler, saturate(uv1), 0.0);
  float in0 = WaterFieldInside(uv0);
  float in1 = WaterFieldInside(uv1);
  return lerp(s1 * in1, s0, in0);
}

// Ripple height (ring 0 only) for finite-difference normal perturbation.
float WaterFieldHeight(float2 world) {
  float2 uv0 = WaterFieldUv(0, world);
  float in0 = WaterFieldInside(uv0);
  return water_field_ring0.SampleLevel(water_field_ring0_sampler, saturate(uv0), 0.0).r * in0;
}

#endif  // RX_WATER_FIELD_HLSLI_
