#include "rhi_bindings.hlsli"
// Content-adaptive shading rate: one thread per rate-image texel scores its
// screen block's luminance detail (sparse 4x4 tap grid over the lit hdr
// color) and relaxes the shading rate where a half/quarter-rate
// reconstruction would stay under a Weber-Fechner error threshold, scaled up
// where screen motion hides the loss. Written this frame, consumed by the
// next frame's scene pass - one frame of staleness is the standard trade.

struct PushData {
  uint2 render_size;
  uint2 rate_size;
  uint texel_size;     // pixels per rate texel (caps.shading_rate_texel)
  float threshold;     // relative luma error allowed for half rate
  float motion_scale;  // threshold multiplier at >= 32px/frame of motion
  uint allow_coarse;   // 1 = the device supports 4x4 fragments
};
PUSH_CONSTANTS(PushData, pc);

[[vk::image_format("r8ui")]] [[vk::binding(0, 0)]] RWTexture2D<uint> rate_image : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D<float4> color : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState color_sampler : register(s1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> motion : register(t2, space0);

// Vulkan fragment size encoding: (log2(w) << 2) | log2(h).
static const uint kRate1x1 = 0x0;
static const uint kRate1x2 = 0x1;
static const uint kRate2x1 = 0x4;
static const uint kRate2x2 = 0x5;
static const uint kRate4x4 = 0xA;

float Luma(float2 uv) {
  float3 c = color.SampleLevel(color_sampler, uv, 0).rgb;
  return dot(c, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= pc.rate_size)) return;

  float2 inv_render = 1.0 / float2(pc.render_size);
  float2 block_origin = float2(id.xy) * pc.texel_size;
  float tap_step = pc.texel_size / 4.0;

  // 4x4 luma taps across the block; mean brightness plus mean absolute
  // horizontal/vertical differences approximate the directional detail a
  // coarser rate would blur away.
  float taps[4][4];
  float mean = 0.0;
  [unroll] for (uint y = 0; y < 4; ++y) {
    [unroll] for (uint x = 0; x < 4; ++x) {
      float2 uv = (block_origin + (float2(x, y) + 0.5) * tap_step) * inv_render;
      taps[y][x] = Luma(uv);
      mean += taps[y][x];
    }
  }
  mean /= 16.0;

  float dx = 0.0;
  float dy = 0.0;
  [unroll] for (uint j = 0; j < 4; ++j) {
    [unroll] for (uint i = 0; i < 3; ++i) {
      dx += abs(taps[j][i + 1] - taps[j][i]);
      dy += abs(taps[i + 1][j] - taps[i][j]);
    }
  }
  dx /= 12.0;
  dy /= 12.0;

  // Weber-Fechner: the eye notices relative error, so the allowance grows
  // with brightness (plus a floor so blacks don't pin to full rate). The
  // allowance is mostly motion-gated: a static camera keeps near-full rate
  // (coarse fragments fight the temporal upscaler's jittered sub-pixel
  // reconstruction and stripe glossy surfaces), while screen motion - where
  // blur and TAA smearing hide the loss - relaxes it hard.
  int2 center_px = min(int2(block_origin + 0.5 * pc.texel_size), int2(pc.render_size) - 1);
  float2 motion_px = motion.Load(int3(center_px, 0)).xy * float2(pc.render_size);
  float motion_relax = lerp(0.25, pc.motion_scale, saturate(length(motion_px) / 16.0));
  float allow = pc.threshold * (mean + 0.02) * motion_relax;

  bool half_x = dx < allow;   // low horizontal detail -> wide fragments
  bool half_y = dy < allow;
  bool quarter = pc.allow_coarse != 0u && dx < allow * 0.4 && dy < allow * 0.4;

  uint rate = kRate1x1;
  if (quarter) rate = kRate4x4;
  else if (half_x && half_y) rate = kRate2x2;
  else if (half_x) rate = kRate2x1;
  else if (half_y) rate = kRate1x2;
  rate_image[id.xy] = rate;
}
