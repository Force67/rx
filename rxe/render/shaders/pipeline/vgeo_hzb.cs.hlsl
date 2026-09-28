#include "rhi_bindings.hlsli"
#include "vgeo_common.hlsli"
// Rebuilds the coarse farthest-depth hi-z from THIS frame's occluders, in
// between the two cull phases: the scene depth attachment (regular opaque
// geometry) merged with the main-pass visibility buffer (virtual geometry
// rasterized so far). One thread per hi-z texel reduces its 8x8 footprint.
// Per pixel the nearest surface (max, reversed z) is the occluder; across the
// footprint the farthest (min) is the only safe bound.

[[vk::binding(0, 0)]] StructuredBuffer<VgeoParams> params_buf : register(t0, space0);
[[vk::binding(1, 0)]] Texture2D<float> scene_depth : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint64_t> visbuffer : register(t2, space0);
[[vk::binding(3, 0)]] RWTexture2D<float> hiz : register(u3, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  VgeoParams p = params_buf[0];
  uint2 hiz_size = uint2(p.hiz.xy);
  if (id.x >= hiz_size.x || id.y >= hiz_size.y) return;

  float farthest = 1.0;
  uint2 base = id.xy * 8;
  for (uint y = 0; y < 8; ++y) {
    for (uint x = 0; x < 8; ++x) {
      uint2 px = min(base + uint2(x, y), uint2(p.width - 1, p.height - 1));
      float scene = scene_depth.Load(int3(px, 0));
      float vgeo = asfloat(uint(visbuffer[px.y * p.width + px.x] >> 32));
      farthest = min(farthest, max(scene, vgeo));
    }
  }
  hiz[id.xy] = farthest;
}
