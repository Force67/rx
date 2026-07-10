// scenehook-rhi demo fragment stage (shared by the vertex and mesh-shader draw
// paths): samples the per-instance texture-array layer, shades into rx's HDR-
// linear scene colour, and writes the R32F depth-export copy so rx's depth-aware
// passes respect the app geometry.
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2DArray atlas : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState atlas_sampler : register(s0, space0);

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 color : COLOR0;
  [[vk::location(1)]] float3 normal : NORMAL0;
  [[vk::location(2)]] float2 uv : TEXCOORD0;
  [[vk::location(3)]] nointerpolation uint layer : LAYER0;
};

struct PsOut {
  float4 scene : SV_Target0;  // RGBA16F scene colour (pre-tonemap, linear)
  float depth : SV_Target1;   // R32F depth-export copy (reversed-z device depth)
};

PsOut main(PsIn i) {
  float3 n = normalize(i.normal);
  float3 l = normalize(float3(0.4, 0.85, 0.3));
  float ndl = saturate(dot(n, l)) * 0.85 + 0.15;
  float3 tex = atlas.Sample(atlas_sampler, float3(i.uv, (float)i.layer)).rgb;
  PsOut o;
  o.scene = float4(i.color * tex * ndl * 1.6, 1.0);
  o.depth = i.pos.z;
  return o;
}
