// Underwater caustics + wave shadows, shared by mesh.ps and mesh_rt.ps. The
// caustics compute pass (water_caustics.cs) refracts a sun-ray grid through the
// water surface onto a reference receiver plane and writes a tiling world-space
// map (env slot 34): R = energy-conserving caustic density (normalized to mean
// 1, so it both brightens AND darkens the sun), G = a soft wave-shadow /
// surface-transmission term. Surfaces below the water rest height modulate their
// direct sun by this; everything above water is a no-op. Gated by
// kFrameFlagWaterCaustics so the slot binds a black dummy and this returns 1
// when the scene did not enable it.
#ifndef RX_WATER_CAUSTICS_HLSLI_
#define RX_WATER_CAUSTICS_HLSLI_

static const uint kFrameWaterCaustics = 1u << 15;  // mirrors kFrameFlagWaterCaustics
static const float kCausticTile = 64.0;  // world tiling (m); mirrors water_caustics.cs kTile

// env set slot 34: RG16F tiling caustic map (R density mean 1, G wave shadow).
[[vk::combinedImageSampler]] [[vk::binding(34, 2)]] Texture2D<float2> water_caustic_map : register(t34, space2);
[[vk::combinedImageSampler]] [[vk::binding(34, 2)]] SamplerState water_caustic_sampler : register(s34, space2);

// Sun-light multiplier for a shaded point. Returns 1 above the rest height or
// when the feature is off, so it can fold straight into the sun shadow term.
// absorption: per-channel Beer coefficient (frame.water_absorption.rgb * .w);
// caustic_params: frame.water_caustics (x intensity, y rest height, z depth-fade).
float WaterCaustic(float3 world_pos, uint flags, float3 absorption, float4 caustic_params) {
  if ((flags & kFrameWaterCaustics) == 0u) return 1.0;
  float depth = caustic_params.y - world_pos.y;  // metres below the rest surface
  if (depth <= 0.0) return 1.0;                   // above water: untouched
  float2 uv = world_pos.xz / kCausticTile;
  float2 c = water_caustic_map.SampleLevel(water_caustic_sampler, uv, 0.0);  // R density, G shadow
  // Caustics wash out with depth: the refracted focus spreads and the water
  // column absorbs it, so fade the (mean-1) pattern back toward flat with the
  // luma-weighted absorption plus an authored term. Fading toward 1 keeps the
  // energy neutral - deep water neither brightens nor darkens on average.
  float absorb = dot(absorption, float3(0.30, 0.59, 0.11));
  float fade = exp(-depth * (absorb + max(caustic_params.z, 0.0)));
  float caustic = lerp(1.0, c.r, saturate(caustic_params.x) * fade);
  float shadow = lerp(1.0, c.g, fade);
  return caustic * shadow;
}

#endif  // RX_WATER_CAUSTICS_HLSLI_
