#include "rhi_bindings.hlsli"
// Separable Gaussian blur for the UI frosted-glass backdrop. Run twice
// (horizontal then vertical) over a downsampled copy of the post-tonemap
// backbuffer to produce the small blurred texture frosted panels sample. Pairs
// with fullscreen.vs (a single screen-covering triangle). The sampler must
// filter linearly, which the pass's clamp sampler does by default.

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D src : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState src_sampler : register(s0, space0);

struct Push {
  float2 dir;  // per-tap UV step along one axis (the other component is 0)
};
PUSH_CONSTANTS(Push, push);

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

float4 main(PsIn input) : SV_Target0 {
  // The same 9-tap Gaussian (sigma ~ 2) in 5 taps: the sampler is bilinear, so
  // one fetch placed between two neighbours returns their weighted sum for free
  // if it sits at the offset their weights balance at. Pairs are (1,2) and
  // (3,4); the result is identical, at 5 fetches per pixel instead of 9.
  const float w0 = 0.2270270270;  // centre
  const float w1 = 0.3162162162;  // taps 1 + 2
  const float w2 = 0.0702702703;  // taps 3 + 4
  const float o1 = 1.3846153846;
  const float o2 = 3.2307692308;
  float2 uv = input.uv;
  float3 c = src.Sample(src_sampler, uv).rgb * w0;
  c += src.Sample(src_sampler, uv + push.dir * o1).rgb * w1;
  c += src.Sample(src_sampler, uv - push.dir * o1).rgb * w1;
  c += src.Sample(src_sampler, uv + push.dir * o2).rgb * w2;
  c += src.Sample(src_sampler, uv - push.dir * o2).rgb * w2;
  return float4(c, 1.0);
}
