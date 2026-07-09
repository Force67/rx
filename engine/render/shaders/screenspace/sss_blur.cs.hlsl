#include "rhi_bindings.hlsli"
// Screen-space subsurface scattering (separable, Jimenez-style): the scene
// pass exported the diffuse-only lighting of skin-flagged materials (rgb) and
// a mask (a). Two passes diffuse it along x then y with a per-channel Gaussian
// profile (red scatters widest - the classic terminator bleed), guided by
// depth so the blur follows the surface instead of leaking across silhouettes.
// The second pass rewrites the scene color in place: color - original + blurred.
struct SssPush {
  uint2 size;
  float2 inv_size;
  float2 dir;         // (1,0) then (0,1), pixels
  float near_plane;
  float width;        // world scattering radius, meters
  float proj_scale;   // pixels per meter at view depth 1
  float max_radius;   // pixels
  uint composite;     // 0 = blur skin -> out, 1 = blur + rewrite scene color
  float strength;
};
PUSH_CONSTANTS(SssPush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_color : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D<float4> src : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState src_sampler : register(s1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float> depth_map : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState depth_sampler : register(s2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D<float4> original : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState original_sampler : register(s3, space0);

// Per-channel normalized widths of the diffusion profile (fraction of the
// full radius). Red travels furthest through skin; blue barely spreads.
static const float3 kChannelSigma = float3(0.38, 0.22, 0.13);
static const int kTaps = 6;  // each side of center

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  float2 uv = (float2(id.xy) + 0.5) * pc.inv_size;

  float4 center_orig = original.SampleLevel(original_sampler, uv, 0.0);
  float mask = center_orig.a;
  float depth0 = depth_map.SampleLevel(depth_sampler, uv, 0.0);
  // Debug (RX_SSS_DEBUG): categorical mask view - green = skin, red = clear.
  if (pc.strength < 0.0) {
    if (pc.composite == 1u) {
      out_color[id.xy] = mask > 0.5 ? float4(0.0, 4.0, 0.0, 1.0)
                                    : float4(4.0 * saturate(mask * 50.0 + 0.1), 0.0, 0.0, 1.0);
    }
    return;
  }
  if (mask < 0.01 || depth0 <= 0.0) {
    if (pc.composite == 0u) out_color[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
    return;
  }

  float view_z = pc.near_plane / max(depth0, 1e-7);
  float radius_px = clamp(pc.width * pc.proj_scale / view_z, 0.0, pc.max_radius);
  float4 center_src = src.SampleLevel(src_sampler, uv, 0.0);
  if (radius_px < 0.75) {
    // Too far away to resolve the scattering: pass through unchanged.
    if (pc.composite == 0u) out_color[id.xy] = center_src;
    return;
  }

  // Depth rejection: a tap more than a few scattering widths in front of or
  // behind the surface belongs to different geometry.
  float depth_reject = 1.0 / max(pc.width * 3.0, 1e-4);

  float3 sum = center_src.rgb;
  float3 weight_sum = float3(1.0, 1.0, 1.0);
  [unroll]
  for (int i = -kTaps; i <= kTaps; ++i) {
    if (i == 0) continue;
    float x = float(i) / float(kTaps);
    float r = x * abs(x);  // denser taps near the center
    float2 tap_uv = uv + pc.dir * (r * radius_px) * pc.inv_size;
    float4 tap = src.SampleLevel(src_sampler, tap_uv, 0.0);
    float tap_depth = depth_map.SampleLevel(depth_sampler, tap_uv, 0.0);
    if (tap_depth <= 0.0) continue;
    float tap_z = pc.near_plane / max(tap_depth, 1e-7);
    float follow = exp2(-(tap_z - view_z) * (tap_z - view_z) * depth_reject * depth_reject);
    float3 w = exp2(-(r * r) / (2.0 * kChannelSigma * kChannelSigma)) * tap.a * follow;
    sum += tap.rgb * w;
    weight_sum += w;
  }
  float3 blurred = sum / weight_sum;

  if (pc.composite == 0u) {
    out_color[id.xy] = float4(blurred, mask);
  } else {
    // Replace the diffuse the scene pass wrote with its diffused version.
    blurred = lerp(center_orig.rgb, blurred, pc.strength);
    float4 color = out_color[id.xy];
    out_color[id.xy] = float4(max(color.rgb - center_orig.rgb + blurred, 0.0), color.a);
  }
}
