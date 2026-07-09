#include "rhi_bindings.hlsli"
// Imposter bake fragment: MRT0 albedo (vertex color) + coverage, MRT1
// bake-space normal. Background stays alpha 0 (atlas cleared beforehand).
struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 color : COLOR0;
};

struct PsOut {
  float4 albedo : SV_Target0;
  float4 normal : SV_Target1;
};

PsOut main(PsIn input) {
  PsOut o;
  o.albedo = float4(input.color.rgb, 1.0);
  o.normal = float4(normalize(input.normal) * 0.5 + 0.5, 1.0);
  return o;
}
