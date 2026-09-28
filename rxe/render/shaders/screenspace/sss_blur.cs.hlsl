#include "rhi_bindings.hlsli"
#include "sss_profile.hlsli"
// Screen-space subsurface scattering (separable): the scene pass exported the
// diffuse-only lighting of skin-flagged materials (rgb) and a PER-PIXEL world
// scatter radius (a; 0 = not skin, else the blood-modulated red mean free path
// x3). Two passes diffuse it along x then y with a per-channel Christensen-
// Burley profile (red scatters widest - the classic terminator bleed), guided
// by depth so the blur follows the surface instead of leaking across
// silhouettes. The second pass rewrites the scene color: color - original +
// blurred. `width` is now a global artist multiplier on the per-pixel radius.
struct SssPush {
  uint2 size;
  float2 inv_size;
  float2 dir;         // (1,0) then (0,1), pixels
  float near_plane;
  float width;        // global multiplier on the per-pixel world radius (1 = as authored)
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

// Per-channel Burley scale (fraction of the sampling radius). Red travels
// furthest through skin; blue barely spreads. Used as the profile's `d`.
static const float3 kChannelD = float3(0.38, 0.22, 0.13);
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
  if (mask < 1e-4 || depth0 <= 0.0) {
    if (pc.composite == 0u) out_color[id.xy] = float4(0.0, 0.0, 0.0, 0.0);
    return;
  }

  // Per-pixel scatter radius (world m) rides the alpha; `width` is a global
  // multiplier for artist control (RX_SSS_WIDTH / reference).
  float radius_world = mask * pc.width;
  float view_z = pc.near_plane / max(depth0, 1e-7);
  float radius_px = clamp(radius_world * pc.proj_scale / view_z, 0.0, pc.max_radius);
  float4 center_src = src.SampleLevel(src_sampler, uv, 0.0);
  if (radius_px < 0.75) {
    // Too far away to resolve the scattering: pass through unchanged.
    if (pc.composite == 0u) out_color[id.xy] = center_src;
    return;
  }

  // Depth rejection: a tap more than a few scattering widths in front of or
  // behind the surface belongs to different geometry.
  float depth_reject = 1.0 / max(radius_world * 3.0, 1e-4);

  // Burley radial PDF weights (finite at r=0, heavy-tailed). Center anchors at
  // r=0; taps evaluate the profile at their normalized radius.
  float3 pdf0 = float3(SssBurleyPdf(0.0, kChannelD.r), SssBurleyPdf(0.0, kChannelD.g),
                       SssBurleyPdf(0.0, kChannelD.b));
  float3 sum = center_src.rgb * pdf0;
  float3 weight_sum = pdf0;
  [unroll]
  for (int i = -kTaps; i <= kTaps; ++i) {
    if (i == 0) continue;
    float x = float(i) / float(kTaps);
    float r = x * abs(x);  // denser taps near the center
    float rn = abs(r);     // normalized profile radius (0..1)
    float2 tap_uv = uv + pc.dir * (r * radius_px) * pc.inv_size;
    float4 tap = src.SampleLevel(src_sampler, tap_uv, 0.0);
    float tap_depth = depth_map.SampleLevel(depth_sampler, tap_uv, 0.0);
    if (tap_depth <= 0.0) continue;
    float tap_z = pc.near_plane / max(tap_depth, 1e-7);
    float follow = exp2(-(tap_z - view_z) * (tap_z - view_z) * depth_reject * depth_reject);
    float3 profile = float3(SssBurleyPdf(rn, kChannelD.r), SssBurleyPdf(rn, kChannelD.g),
                            SssBurleyPdf(rn, kChannelD.b));
    float3 w = profile * step(1e-4, tap.a) * follow;  // tap.a>0 masks skin pixels
    sum += tap.rgb * w;
    weight_sum += w;
  }
  float3 blurred = sum / max(weight_sum, 1e-6);

  if (pc.composite == 0u) {
    out_color[id.xy] = float4(blurred, mask);
  } else {
    // Replace the diffuse the scene pass wrote with its diffused version.
    blurred = lerp(center_orig.rgb, blurred, pc.strength);
    float4 color = out_color[id.xy];
    out_color[id.xy] = float4(max(color.rgb - center_orig.rgb + blurred, 0.0), color.a);
  }
}
