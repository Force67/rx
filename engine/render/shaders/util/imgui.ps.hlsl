// Dear ImGui pixel stage: vertex colour modulated by the bound texture (font
// atlas or a user texture). One combined image sampler at set 0, binding 0,
// bound per draw by the backend from the ImDrawCmd's texture id.
#include "rhi_bindings.hlsli"

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D tex : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState smp : register(s0, space0);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 col : COLOR0;
};

float4 main(PsIn i) : SV_Target {
  return i.col * tex.Sample(smp, i.uv);
}
