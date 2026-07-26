// Linear-filtered fullscreen copy, the d3d12 backend's BlitMip lowering
// (D3D12 has no filtered image copy). Pairs with fullscreen.vs.
//
// Slang's Sampler2D is one combined-image-sampler binding on spirv and
// auto-splits into t0/s0 on the hlsl->dxil path, replacing the hlsl twin's
// [[vk::combinedImageSampler]] + register(t0/s0) double annotation.
[[vk::binding(0, 0)]] Sampler2D src;

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

[shader("fragment")]
float4 main(PsIn input) : SV_Target0 {
  return src.SampleLevel(input.uv, 0.0);
}
