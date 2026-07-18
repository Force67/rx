#include "rhi_bindings.hlsli"
#include "cloudscape_noise.hlsli"
// Cloudscape high-frequency detail volume, baked once: 32^3 RGBA8 tiling in all
// three axes. R,G,B carry inverted-Worley fbm at ~2, 4, 8 cells across the
// texture; the raymarcher scrolls this at a tighter scale than the base shape
// to fray cloud edges into wisps. A is unused (1). Same wrapped-lattice tiling
// as the base volume so the fine detail repeats seamlessly.

[[vk::image_format("rgba8")]] [[vk::binding(0, 0)]]
RWTexture3D<float4> out_noise : register(u0, space0);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
  uint3 dims;
  out_noise.GetDimensions(dims.x, dims.y, dims.z);
  if (any(id >= dims)) return;
  float3 uvw = (float3(id) + 0.5) / float3(dims);

  float r = cs_worley3_fbm(uvw, 2.0);
  float g = cs_worley3_fbm(uvw, 4.0);
  float b = cs_worley3_fbm(uvw, 8.0);
  out_noise[id] = float4(r, g, b, 1.0);
}
