// Shoreline wetting material response, shared by mesh.ps and mesh_rt.ps. The
// world-space wetness field (shore_wetting.cs) drives a wet look on the opaque
// surface: darker albedo, glossier roughness, a touch more dielectric
// reflectance. Gated by kFrameShoreWetting so the slot binds a 1x1 dummy and
// this is a no-op when the scene did not enable it.
#ifndef RX_SHORE_WETTING_HLSLI_
#define RX_SHORE_WETTING_HLSLI_

static const uint kFrameShoreWetting = 1u << 14;  // mirrors kFrameFlagShoreWetting

// env set slot 33: the R16F wetness field (0 dry .. 1 soaked), linear-clamped.
[[vk::combinedImageSampler]] [[vk::binding(33, 2)]] Texture2D<float> shore_wetness_map : register(t33, space2);
[[vk::combinedImageSampler]] [[vk::binding(33, 2)]] SamplerState shore_wetness_sampler : register(s33, space2);

// frame.shore_field: xy field min-corner world xz, z 1/extent (1/m), w rest
// water height. Returns 0..1; the field's own texel filtering keeps the
// boundary a soft band.
float ShoreWetness(float3 world_pos, uint flags, float4 shore_field) {
  if ((flags & kFrameShoreWetting) == 0u) return 0.0;
  float2 uv = (world_pos.xz - shore_field.xy) * shore_field.z;
  if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;
  float wet = saturate(shore_wetness_map.SampleLevel(shore_wetness_sampler, uv, 0.0));
  // The field is 2D: open water reads fully wet, which would soak a floating
  // hull to its top. Waves only splash so high — fade the response out above
  // the reach of the swell so decks and cube tops stay dry.
  wet *= saturate(1.0 - (world_pos.y - shore_field.w - 0.6) / 1.2);
  return wet;
}

// The wet response itself (darker albedo, glossier roughness, a small f0 lift)
// is applied inline in ShadeSurface, at the points those values are formed.

#endif  // RX_SHORE_WETTING_HLSLI_
