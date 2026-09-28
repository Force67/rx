// render2d sprite pass - fragment stage. Samples the bound atlas (an sRGB
// texture, so the sampler returns linear), multiplies the per-sprite linear
// tint, and writes straight into the target (rx's HDR scene colour on the
// direct path, or the module's albedo target on the lit path). Alpha-blended by
// the pipeline; fully transparent texels are discarded so overlapping sprites
// never leave a coverage smear.

[[vk::combinedImageSampler]] [[vk::binding(1, 0)]]
Texture2D atlas : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]]
SamplerState atlas_sampler : register(s1, space0);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 color : COLOR0;
};

float4 main(PsIn i) : SV_Target0 {
  float4 tex = atlas.Sample(atlas_sampler, i.uv);
  float4 c = tex * i.color;
  if (c.a < 0.004) discard;
  return c;
}
