#include "rhi_bindings.hlsli"
#include "post/tonemap_ops.hlsli"

// Reference comparison, run on the SCENE-LINEAR image immediately before
// exposure/bloom/tonemap. Placing it here is the whole point: reference and
// render then travel the same colour and tone-mapping path, so what you are
// looking at is a material difference and not a display difference.
//
// Modes (mirrors render::ReferenceCompare::Mode):
//   0 off   1 side by side   2 wipe   3 linear difference
//   4 display-referred difference   5 reference only
//
// The pass also accumulates a per-region error metric into a small storage
// buffer, which is what makes the automated fitting in the lookdev tool a
// measurement rather than an eyeball.
struct ComparePush {
  uint2 size;
  float2 inv_size;
  float2 ref_uv_scale;   // reference alignment: uv = (screen_uv - offset) / scale
  float2 ref_uv_offset;
  float ref_exposure;    // linear multiplier applied to the reference
  float difference_gain;
  float split;           // 0..1 screen x for modes 1 and 2
  uint mode;
  uint tonemap_op;       // render::TonemapOperator, for mode 4
  uint region;           // 0 = whole frame, 1..4 = isolate that mask channel
  uint stats;            // 1 = accumulate the error metric
  float exposure_scale;  // the frame's resolved exposure, so mode 4 matches
  uint stats_base;       // first word of this frame's slot in the stats ring
};
PUSH_CONSTANTS(ComparePush, pc);

// Separate source and destination: the side-by-side mode resamples the render,
// which is a read of a texel another thread is writing if they share one image.
[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2D<float4> scene : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState scene_sampler : register(s4, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D<float4> reference : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState reference_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float4> region_mask : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState region_sampler : register(s2, space0);
// 5 buckets (whole frame, then the mask's four channels) x 4 accumulators
// (squared error, absolute error, reference luminance, pixel count) x 2 words.
// Fixed point (x 65536) because atomics on float are not portable, and 64 bit
// because a single u32 at that scale wraps after 65536 masked pixels - a 256x256
// region. Every measurement worth making is larger than that, so the wrap used
// to turn the whole metric into noise.
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> stats_buffer : register(u3, space0);

static const float kStatScale = 65536.0;
static const uint kStatWholeFrame = 0u;  // bucket 0 is the frame, not a mask channel

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Accumulates one non-negative sample into a 64-bit fixed-point slot. `value`
// is clamped at zero first: a tonemap can return a slightly negative channel,
// and casting that to uint wraps to ~4e9 and destroys the accumulator.
void StatAdd(uint slot, float value) {
  uint amount = (uint)(max(value, 0.0) * kStatScale);
  if (amount == 0u) return;
  uint prev;
  InterlockedAdd(stats_buffer[slot], amount, prev);
  // Unsigned wrap is the carry: the sum only goes down when it overflowed.
  if (prev + amount < prev) InterlockedAdd(stats_buffer[slot + 1u], 1u);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  float2 uv = (float2(id.xy) + 0.5) * pc.inv_size;
  float3 render = scene.SampleLevel(scene_sampler, uv, 0.0).rgb;
  if (pc.mode == 0u) {
    out_color[id.xy] = float4(render, 1.0);
    return;
  }

  // Reference alignment. Outside the aligned rectangle there is no reference,
  // so the render passes through untouched rather than being compared against
  // black - a black "difference" outside the capture would read as a match.
  float2 ref_uv = (uv - pc.ref_uv_offset) / max(pc.ref_uv_scale, 1e-5);
  bool inside = all(ref_uv >= 0.0) && all(ref_uv <= 1.0);
  float3 ref = inside ? reference.SampleLevel(reference_sampler, ref_uv, 0.0).rgb * pc.ref_exposure
                      : float3(0.0, 0.0, 0.0);

  // Region isolation. The mask's channels are skin / eyes / lips / teeth; a
  // material fitted against "the whole face" is fitted against four materials
  // at once and converges on none of them.
  float mask = 1.0;
  if (pc.region > 0u) {
    float4 m = region_mask.SampleLevel(region_sampler, uv, 0.0);
    mask = pc.region == 1u ? m.r : (pc.region == 2u ? m.g : (pc.region == 3u ? m.b : m.a));
  }

  float3 outp = render;
  if (!inside) {
    outp = render;
  } else if (pc.mode == 1u) {
    // Side by side: the render squeezed into the left half, the reference into
    // the right, so both are visible whole at the same scale.
    float half_split = saturate(pc.split);
    if (uv.x < half_split) {
      float2 s = float2(uv.x / max(half_split, 1e-4), uv.y);
      outp = scene.SampleLevel(scene_sampler, s, 0.0).rgb;
    } else {
      float2 s = float2((uv.x - half_split) / max(1.0 - half_split, 1e-4), uv.y);
      float2 r = (s - pc.ref_uv_offset) / max(pc.ref_uv_scale, 1e-5);
      outp = reference.SampleLevel(reference_sampler, saturate(r), 0.0).rgb * pc.ref_exposure;
    }
  } else if (pc.mode == 2u) {
    outp = uv.x < saturate(pc.split) ? render : ref;
    // A one-pixel guide so the wipe edge is unambiguous under a difference.
    if (abs(uv.x - saturate(pc.split)) < pc.inv_size.x) outp = float3(0.0, 4.0, 0.0);
  } else if (pc.mode == 3u) {
    outp = abs(render - ref) * pc.difference_gain * mask;
  } else if (pc.mode == 4u) {
    // Display-referred: both through the frame's own exposure and tonemap, so
    // the difference is weighted the way the eye will actually see it.
    float3 a = TonemapApply(render * pc.exposure_scale, pc.tonemap_op);
    float3 b = TonemapApply(ref * pc.exposure_scale, pc.tonemap_op);
    outp = abs(a - b) * pc.difference_gain * mask;
  } else if (pc.mode == 5u) {
    outp = ref;
  }
  out_color[id.xy] = float4(outp, 1.0);

  if (pc.stats != 0u && inside) {
    // Error is measured on the DISPLAY-referred pair: a linear metric is
    // dominated by the highlights and would happily trade a wrong terminator
    // for a slightly better specular core.
    float3 a = TonemapApply(render * pc.exposure_scale, pc.tonemap_op);
    float3 b = TonemapApply(ref * pc.exposure_scale, pc.tonemap_op);
    float3 d = a - b;
    const float sq = dot(d, d);
    const float abs_err = dot(abs(d), 1.0.xxx);
    const float luma = Luma(b);
    float4 m = saturate(region_mask.SampleLevel(region_sampler, uv, 0.0));
    // Bucket 0 is the WHOLE FRAME, which the mask channels cannot stand in for:
    // without it, asking for Region::kAll answered with whatever the red
    // channel happened to cover.
    for (uint r = 0; r <= 4u; ++r) {
      float w = r == kStatWholeFrame ? 1.0
                                     : (r == 1u ? m.r : (r == 2u ? m.g : (r == 3u ? m.b : m.a)));
      if (w <= 0.001) continue;
      uint base = pc.stats_base + r * 8u;
      StatAdd(base + 0u, sq * w);
      StatAdd(base + 2u, abs_err * w);
      StatAdd(base + 4u, luma * w);
      StatAdd(base + 6u, w);
    }
  }
}
