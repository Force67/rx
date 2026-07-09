// Sample-0 resolve of the multisampled prepass guides (normals+roughness and
// the raw reversed-z export) into single-sampled images for every downstream
// consumer (AO, SSR, froxels, hi-z). Averaging would invent phantom values at
// silhouettes - a depth between two surfaces, a bent normal - so one real
// covered sample wins instead. Motion resolves with the color after the scene
// pass (the sky writes it there).
#include "rhi_bindings.hlsli"

[[vk::binding(0, 0)]] Texture2DMS<float4> ms_normals : register(t0, space0);
[[vk::binding(1, 0)]] Texture2DMS<float> ms_depth : register(t1, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(2, 0)]] RWTexture2D<float4> out_normals : register(u2, space0);
[[vk::image_format("r32f")]] [[vk::binding(3, 0)]] RWTexture2D<float> out_depth : register(u3, space0);

struct PushData {
  uint width;
  uint height;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= push.width || id.y >= push.height) return;
  int2 p = int2(id.xy);
  out_normals[p] = ms_normals.Load(p, 0);
  out_depth[p] = ms_depth.Load(p, 0);
}
