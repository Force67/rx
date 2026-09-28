#include "rhi_bindings.hlsli"
#include "geometry/hair_transmittance.hlsli"

// Deep opacity map accumulation. Each hair fragment adds itself to every layer
// whose boundary lies at or beyond its own depth past the front-most fibre, so
// layer k ends up holding "how many fibres are within b_k of the surface". The
// pass runs with additive blending and NO depth test: every fibre along the ray
// has to be counted, including the ones the front one would occlude.
// Identical layout to hair.vs's DrawPush - one vertex shader serves this pass
// and the lit draw, and a SPIR-V push block has to match across the stages of
// one pipeline.
struct DrawPush {
  column_major float4x4 view_proj;
  float4 camera;      // xyz light "eye" (ribbons face the light in this pass), w = ribbon width
  float4 sun;
  float4 sun_color;   // w = clump radius (hair.vs reads it)
  float4 tint;        // w = children count (hair.vs reads it)
};
PUSH_CONSTANTS(DrawPush, pc);

[[vk::binding(3, 0)]] ConstantBuffer<HairVolume> hair_volume : register(b3, space0);

[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float> front_depth : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState front_sampler : register(s2, space0);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 tangent : TANGENT;
  [[vk::location(1)]] float3 world_pos : POSITION1;
  [[vk::location(2)]] float along : TEXCOORD0;
  [[vk::location(3)]] float3 color : COLOR0;
  [[vk::location(4)]] float side : TEXCOORD1;
};

float4 main(PsIn input) : SV_Target0 {
  float2 dims;
  front_depth.GetDimensions(dims.x, dims.y);
  float2 uv = input.pos.xy / dims;
  float range = hair_volume.transmittance.depth_range;
  float d0 = HairDomDepthMetres(front_depth.SampleLevel(front_sampler, uv, 0.0), range);
  float d = HairDomDepthMetres(input.pos.z, range);
  float u = max(d - d0, 0.0) / max(hair_volume.transmittance.layer_depth, 1e-4);

  // A ribbon is a flat stand-in for a round fibre, so a fragment near its edge
  // covers less of the light ray than one at its centre. Weighting by that
  // keeps a clump's count proportional to the hair actually in the way instead
  // of to how many quads happened to overlap.
  float coverage = sqrt(saturate(1.0 - input.side * input.side));

  return float4(u <= RX_HAIR_DOM_LAYERS.x ? coverage : 0.0,
                u <= RX_HAIR_DOM_LAYERS.y ? coverage : 0.0,
                u <= RX_HAIR_DOM_LAYERS.z ? coverage : 0.0,
                u <= RX_HAIR_DOM_LAYERS.w ? coverage : 0.0);
}
