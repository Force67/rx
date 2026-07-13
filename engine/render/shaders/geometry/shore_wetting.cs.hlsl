// Wave-driven shoreline wetting field. A camera-following, world-space R16F
// texture (0 dry .. 1 soaked) that mesh.ps darkens/smooths opaque surfaces
// with. Each texel compares the water surface height at its world xz against
// the terrain height there: submerged texels latch to 1, exposed texels dry
// out exponentially, so an individual wave's reach up the beach stays wet for a
// beat and then fades. Ping-ponged: this pass reads the previous field
// (resampled at the previous origin, which recenters it as the camera moves)
// and writes the new one.

#include "rhi_bindings.hlsli"
#include "water_waves.hlsli"  // GerstnerWave: the same field mesh.vs displaces with

struct PushData {
  float4 field;       // origin xz (field min corner), extent (m), 1/extent
  float4 prev_field;  // previous origin xz, unused zw
  float4 island;      // analytic beach: center xz, gaussian sigma (m), peak (m)
  float4 params;      // time (s), frame dt (s), drying time (s), fft flag
};
PUSH_CONSTANTS(PushData, push);

[[vk::binding(0, 0)]] RWTexture2D<float> wetness_out : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D<float> wetness_prev : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState wetness_prev_sampler : register(s1, space0);
// FFT ocean displacement (env-owned, GENERAL layout): sampled for the water
// height when the ocean is active so the wet line follows the real surface;
// the Gerstner fallback runs otherwise. A 1x1 dummy is bound when inactive.
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float4> ocean_disp : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState ocean_disp_sampler : register(s2, space0);

static const uint kResolution = 1024u;  // mirrors ShoreWetting::kResolution
static const float kWetMargin = 1.0;    // splash/capillary rise above the waterline (m)

// Height source. The demo beach is a radial gaussian dome that peaks `peak`
// above the rest water at its center and falls to -peak far out, so the
// waterline sits near sigma*sqrt(2 ln2). Mirrors the mesh built in
// demo_scenes.cc. A captured top-down heightmap would replace this function
// (the rest of the pass is agnostic to how the terrain height is produced).
float TerrainHeight(float2 world) {
  float2 d = world - push.island.xy;
  float sigma = max(push.island.z, 0.1);
  float g = exp(-dot(d, d) / (2.0 * sigma * sigma));
  return push.island.w * (2.0 * g - 1.0);
}

// Water surface height at a rest xz. Rest level is 0 in the water demo; the
// Gerstner offset ignores horizontal chop (a height-field approximation).
float WaterHeight(float2 world) {
  if (push.params.w > 0.5) {
    return ocean_disp.SampleLevel(ocean_disp_sampler, world / kOceanPatchSize, 0.0).y;
  }
  float3 normal;
  float crest;
  return GerstnerWave(world, push.params.x, normal, crest).y;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= kResolution || id.y >= kResolution) return;
  float texel = push.field.z / float(kResolution);
  float2 world = push.field.xy + (float2(id.xy) + 0.5) * texel;

  // Soaked at or below the water surface, fading over a small margin above it
  // (splash + capillary rise), so the instantaneous wet zone is a soft band
  // that the waves push up and down the slope rather than a razor-thin line.
  float gap = TerrainHeight(world) - WaterHeight(world);  // sand height above water
  float submerged = 1.0 - smoothstep(0.0, kWetMargin, gap);

  // History from the previous field, resampled at the previous origin so the
  // wet band stays put in world space as the camera (and the field) moves.
  float2 prev_uv = (world - push.prev_field.xy) * push.field.w;
  float prev = 0.0;
  if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
    prev = wetness_prev.SampleLevel(wetness_prev_sampler, prev_uv, 0.0);
  }

  // Freshly submerged snaps to soaked; otherwise the last value dries away.
  float decay = exp(-push.params.y / max(push.params.z, 0.01));
  wetness_out[id.xy] = saturate(max(submerged, prev * decay));
}
