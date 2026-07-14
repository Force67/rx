#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
#include "gi/sh.hlsli"
// RCGI M2 upscale + temporal + SH resolve (full render resolution). Writes the
// `rcgi_irradiance` transient (env set 2 slot 35) the forward pass folds into
// the indirect-diffuse term. Steps:
//   1. 4-tap poisson bilateral upsample of the denoised gather-res SH, weighted
//      by full-res vs gather-res depth/normal agreement (nearest-valid fallback).
//   2. Evaluate the upsampled SH with the FULL-RES pixel normal -> RGB irradiance
//      (restores normal-map detail); clamp negative lobes.
//   3. Temporal filter (motion reproject, per-pixel sample counter in alpha,
//      neighborhood clamp) into a persistent history image.
// See RCGI.md section 5.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> irradiance_out : register(u0, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(1, 0)]] RWTexture2D<float4> hist_out : register(u1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> sh_r_in : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float4> sh_g_in : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float4> sh_b_in : register(t4, space0);
[[vk::binding(5, 0)]] Texture2D<float> depth_map : register(t5, space0);
[[vk::binding(6, 0)]] Texture2D<float4> normal_map : register(t6, space0);
[[vk::binding(7, 0)]] Texture2D<float2> motion_map : register(t7, space0);
[[vk::binding(8, 0)]] Texture2D<float4> hist_in : register(t8, space0);

struct PushData {
  uint4 dims;    // full_w, full_h, gather_w, gather_h
  float4 params; // near_plane, intensity, max_history, reset
  uint4 misc;    // frame_index, pad...
};
PUSH_CONSTANTS(PushData, push);

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 full = push.dims.xy;
  if (id.x >= full.x || id.y >= full.y) return;

  float depth = depth_map.Load(int3(id.xy, 0));
  if (depth <= 0.0) {  // sky: zero irradiance, reset history
    irradiance_out[id.xy] = 0.0.xxxx;
    hist_out[id.xy] = 0.0.xxxx;
    return;
  }

  float near = push.params.x;
  float cz = near / depth;
  float3 cn = RcgiOctDecode(normal_map.Load(int3(id.xy, 0)).xy);

  uint2 gsz = push.dims.zw;
  float2 uv = (float2(id.xy) + 0.5) / float2(full);
  float2 gpix = uv * float2(gsz) - 0.5;

  // 4-tap bilinear/poisson bilateral upsample. Weight each tap by its gather-res
  // depth/normal agreement with the full-res center; fall back to nearest valid.
  const int2 taps[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};
  int2 base = int2(floor(gpix));
  float2 fr = gpix - float2(base);
  float bw[4] = {(1.0 - fr.x) * (1.0 - fr.y), fr.x * (1.0 - fr.y), (1.0 - fr.x) * fr.y,
                 fr.x * fr.y};

  Sh2 sh = ShZero();
  float wsum = 0.0;
  // Track evaluated irradiance of each tap for the neighborhood clamp.
  float3 mn = 1e30.xxx, mx = 0.0.xxx;
  [unroll]
  for (uint i = 0; i < 4; ++i) {
    int2 q = clamp(base + taps[i], int2(0, 0), int2(gsz) - 1);
    // reconstruct the tap's full-res sample point for depth/normal agreement
    float2 quv = (float2(q) + 0.5) / float2(gsz);
    int2 qfp = clamp(int2(quv * float2(full)), int2(0, 0), int2(full) - 1);
    float qd = depth_map.Load(int3(qfp, 0));
    if (qd <= 0.0) continue;
    float qz = near / qd;
    float3 qn = RcgiOctDecode(normal_map.Load(int3(qfp, 0)).xy);
    float wz = exp(-abs(cz - qz) / max(cz, 0.1) * 8.0);
    float wn = pow(saturate(dot(cn, qn)), 8.0);
    float w = max(bw[i], 1e-3) * wz * wn;

    Sh2 tap;
    tap.r = sh_r_in.Load(int3(q, 0));
    tap.g = sh_g_in.Load(int3(q, 0));
    tap.b = sh_b_in.Load(int3(q, 0));
    sh.r += tap.r * w; sh.g += tap.g * w; sh.b += tap.b * w;
    wsum += w;

    float3 tap_irr = max(ShIrradiance(tap, cn), 0.0.xxx);
    mn = min(mn, tap_irr);
    mx = max(mx, tap_irr);
  }
  // No valid tap at all: every gather-res representative of this full-res pixel
  // was sky (thin geometry over a sky background). mn/mx are still inverted
  // (1e30 / 0), so the neighborhood clamp below would produce non-finite FP16.
  // Treat it as a disocclusion: emit zero irradiance and reset history, skipping
  // the temporal clamp entirely.
  if (wsum <= 1e-4) {
    irradiance_out[id.xy] = 0.0.xxxx;
    hist_out[id.xy] = 0.0.xxxx;
    return;
  }
  float inv = 1.0 / wsum; sh.r *= inv; sh.g *= inv; sh.b *= inv;

  // Evaluate with the full-res normal -> irradiance (restores normal detail).
  float3 cur = max(ShIrradiance(sh, cn), 0.0.xxx);

  // Temporal filter: reproject through the motion vector, per-pixel counter in
  // history alpha, neighborhood clamp against the upsample taps to kill ghosts.
  float2 prev_uv = uv + motion_map.Load(int3(id.xy, 0));
  bool valid = push.params.w == 0.0 && all(prev_uv > 0.0) && all(prev_uv < 1.0);
  float3 accum = cur;
  float n = 1.0;
  if (valid) {
    int2 pp = clamp(int2(prev_uv * float2(full)), int2(0, 0), int2(full) - 1);
    float4 h = hist_in.Load(int3(pp, 0));
    // Neighborhood clamp with a generous margin: the taps come from the same
    // denoised frame (a tight box would pin history to the noisy current sample
    // and defeat temporal integration), so widen it — GI is low frequency and
    // TAA still runs after us. Bounds gross ghosts without killing convergence.
    float3 margin = (mx - mn) * 0.5 + mx * 0.6 + 0.03;
    float3 hist_c = clamp(h.rgb, mn - margin, mx + margin);
    n = min(h.a + 1.0, push.params.z);
    float w = 1.0 / n;
    accum = lerp(hist_c, cur, w);
  }

  // Final guard: history must never go non-finite (a single NaN/Inf would
  // poison every subsequent temporal frame). Clamp to a finite, non-negative
  // FP16 range and scrub any stray non-finite lanes.
  accum = clamp(accum, 0.0.xxx, 65504.0.xxx);
  if (any(isnan(accum)) || any(isinf(accum))) { accum = 0.0.xxx; n = 1.0; }

  hist_out[id.xy] = float4(accum, n);
  irradiance_out[id.xy] = float4(accum * push.params.y, 1.0);  // apply intensity once
}
