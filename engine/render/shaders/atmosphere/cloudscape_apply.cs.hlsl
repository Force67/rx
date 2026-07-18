#include "rhi_bindings.hlsli"
// Cloudscape apply: upsample the persistent half-res cloud buffer over the lit
// scene. The buffer stores premarched (scatter, transmittance); texels the
// scene occluded during the march hold the identity (0, 1), so a plain
// bilinear tap can't bleed cloud onto foreground geometry beyond a soft
// half-res halo -- no bilateral weights needed.

struct ApplyPush {
  uint2 full_size;
  uint2 half_size;
  float flash;  // lightning 0..1: boosts the deck's scatter this frame
  float _pad;
};
PUSH_CONSTANTS(ApplyPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> color_in : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float4> cloud_in : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState cloud_sampler : register(s2, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.full_size.x || id.y >= pc.full_size.y) return;
  int2 px = int2(id.xy);
  float3 scene = color_in.Load(int3(px, 0)).rgb;
  float2 uv = (float2(px) + 0.5) / float2(pc.full_size);
  // Small tent over the half-res buffer: pixels of a 4x4 refresh block sit at
  // different cycle ages, and clouds are soft enough that a gentle spatial
  // filter erases the residual grid without eating any real silhouette.
  float2 texel = 1.0 / float2(pc.half_size);
  float4 cloud = cloud_in.SampleLevel(cloud_sampler, uv, 0.0) * 0.4;
  cloud += cloud_in.SampleLevel(cloud_sampler, uv + float2(texel.x, 0.0), 0.0) * 0.15;
  cloud += cloud_in.SampleLevel(cloud_sampler, uv - float2(texel.x, 0.0), 0.0) * 0.15;
  cloud += cloud_in.SampleLevel(cloud_sampler, uv + float2(0.0, texel.y), 0.0) * 0.15;
  cloud += cloud_in.SampleLevel(cloud_sampler, uv - float2(0.0, texel.y), 0.0) * 0.15;
  // Lightning lives here, not in the march: the flash rises and dies far
  // faster than the 16-frame refresh cycle, so boosting the amortized march
  // would print the refresh grid (and bake flash frames into the history).
  // A full-res scatter boost flashes the whole deck from within, instantly,
  // slightly blue-white the way the channel actually radiates.
  float3 flash_tint = float3(0.92, 0.96, 1.1);
  float3 cloud_rgb = cloud.rgb * (1.0 + pc.flash * 7.0 * flash_tint);
  out_image[px] = float4(scene * cloud.a + cloud_rgb, 1.0);
}
