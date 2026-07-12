// render2d lit-composite - fragment stage. Multiplies the unlit sprite albedo
// by the accumulated 2D light (ambient + light pools) and blends the result
// over rx's scene colour using the albedo's coverage alpha, so uncovered pixels
// keep the background. The bright light pools push past 1.0 in the HDR target,
// which rx's bloom then blooms - the "modern 2D" look.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]]
Texture2D albedo : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]]
SamplerState albedo_sampler : register(s0, space0);

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]]
Texture2D light : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]]
SamplerState light_sampler : register(s1, space0);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 main(PsIn i) : SV_Target0 {
  float4 a = albedo.Sample(albedo_sampler, i.uv);
  float3 l = light.Sample(light_sampler, i.uv).rgb;
  return float4(a.rgb * l, a.a);
}
