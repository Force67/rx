// Dear ImGui pixel stage: vertex colour modulated by the bound texture (font
// atlas or a user texture), composited over an optional frosted backdrop.
//
// Binding 1 is the pre-blurred copy of the scene behind the UI (the renderer's
// ui_blur pass). Where the UI covers a pixel, the sharp scene underneath is
// replaced by that blurred read before the panel's own translucent tint goes on
// top - CSS backdrop-filter, so a window at 80% alpha reads as frosted glass
// instead of a grey wash. Coverage comes from the vertex alpha (imgui encodes
// its rounded-corner antialiasing there, so corners stay round) scaled by
// push.frost, saturated: a panel body frosts fully, an input field's 20% fill
// only slightly. frost = 0 leaves the output at plain alpha blending.
//
// Output is premultiplied (the pipeline blends one/oneMinusSrcAlpha) because
// tint and backdrop carry different alphas and have to be resolved here.
#include "rhi_bindings.hlsli"

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D tex : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState smp : register(s0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D backdrop : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState backdrop_smp
    : register(s1, space0);

struct PushData {
  float2 scale;
  float2 translate;
  float2 inv_target_size;
  float frost;
};
PUSH_CONSTANTS(PushData, push);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 col : COLOR0;
};

float4 main(PsIn i) : SV_Target {
  float4 sampled = tex.Sample(smp, i.uv);
  float4 tint = i.col * sampled;

  // Uniform across the draw, so the backdrop fetch costs nothing at all on the
  // paths that bound none (frost = 0 leaves plain premultiplied alpha).
  if (push.frost <= 0.0) return float4(tint.rgb * tint.a, tint.a);

  float coverage = saturate(i.col.a * push.frost) * sampled.a;
  float3 frosted = backdrop.Sample(backdrop_smp, i.pos.xy * push.inv_target_size).rgb;

  // tint over (backdrop at `coverage`) over the destination, premultiplied.
  float behind = coverage * (1.0 - tint.a);
  return float4(tint.rgb * tint.a + frosted * behind, tint.a + behind);
}
