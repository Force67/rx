#include "rhi_bindings.hlsli"
// Imposter bake fragment: MRT0 albedo + coverage, MRT1 bake-space normal.
// Albedo comes from the submesh's base-colour map, alpha-tested at the
// material's cutoff, so a masked foliage sheet bakes its leaf shape and not
// the rectangle it is drawn on. Untextured submeshes fall back to the vertex
// colour. Background stays alpha 0 (the atlas was cleared to it).
struct BakePush {
  column_major float4x4 view_proj;
  float alpha_cutoff;  // 0 = no cutout
  float textured;      // 0 = albedo from the vertex colour
  float2 pad;
};
PUSH_CONSTANTS(BakePush, pc);

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D base_color : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState base_sampler : register(s0, space0);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 color : COLOR0;
  [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct PsOut {
  float4 albedo : SV_Target0;
  float4 normal : SV_Target1;
};

PsOut main(PsIn input) {
  float4 albedo = input.color;
  if (pc.textured > 0.5) {
    albedo = base_color.Sample(base_sampler, input.uv);
    if (pc.alpha_cutoff > 0.0 && albedo.a < pc.alpha_cutoff) discard;
  }
  PsOut o;
  o.albedo = float4(albedo.rgb, 1.0);
  o.normal = float4(normalize(input.normal) * 0.5 + 0.5, 1.0);
  return o;
}
