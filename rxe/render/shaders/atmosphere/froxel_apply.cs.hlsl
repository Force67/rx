#include "rhi_bindings.hlsli"
// Froxel volumetric lighting, pass 3: composite onto the lit scene. Trilinear
// sample of the integrated volume at each pixel's depth; sky pixels take the
// last slice (the whole volume is in front of them).
struct ApplyPush {
  float near_plane;
  float far_plane;
  uint2 size;
};
PUSH_CONSTANTS(ApplyPush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> lit : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture3D<float4> integrated : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState integrated_sampler : register(s1, space0);
[[vk::binding(2, 0)]] Texture2D<float> depth_map : register(t2, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= pc.size)) return;
  float depth = depth_map.Load(int3(id.xy, 0));
  // Reversed-infinite: view z = near / ndc depth; sky (0) -> beyond the volume.
  float view_z = depth > 1e-7 ? pc.near_plane / depth : pc.far_plane * 2.0;
  float slice = saturate(log2(max(view_z, pc.near_plane) / pc.near_plane) /
                         log2(pc.far_plane / pc.near_plane));
  float2 uv = (float2(id.xy) + 0.5) / float2(pc.size);
  float4 fog = integrated.SampleLevel(integrated_sampler, float3(uv, slice), 0.0);

  float4 color = lit[id.xy];
  lit[id.xy] = float4(color.rgb * fog.a + fog.rgb, color.a);
}
