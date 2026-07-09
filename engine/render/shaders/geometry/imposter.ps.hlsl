#include "rhi_bindings.hlsli"
// Imposter billboard fragment: alpha-tested atlas sample shaded with the
// baked normal against the sun (wrapped) + flat ambient.
struct DrawPush {
  column_major float4x4 view_proj;
  float4 camera;
  float4 sun;
  float4 sun_color;
  float radius;
  float center_y;
  float grid;
  float pad0;
};
PUSH_CONSTANTS(DrawPush, pc);

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D albedo_atlas : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState albedo_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D normal_atlas : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState normal_sampler : register(s2, space0);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float3 world_pos : POSITION1;
};

float4 main(PsIn input) : SV_Target0 {
  float4 albedo = albedo_atlas.Sample(albedo_sampler, input.uv);
  if (albedo.a < 0.5) discard;
  float3 n = normalize(normal_atlas.Sample(normal_sampler, input.uv).xyz * 2.0 - 1.0);
  float3 l = normalize(-pc.sun.xyz);
  float ndl = saturate((dot(n, l) + 0.4) / 1.4);  // wrapped: baked normals are coarse
  float3 li = pc.sun_color.rgb * pc.sun.w;
  float3 color = albedo.rgb * (li * ndl * 0.32 + pc.sun_color.w);
  return float4(color, 1.0);
}
