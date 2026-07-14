#include "rhi_bindings.hlsli"
#include "gi/sdf_trace.hlsli"
// Resets one clip's slab of the global SDF clipmap before (re)composition: far
// positive distance everywhere, zero surface colour. Voxels no instance touches
// stay "far" (empty space), never garbage.

[[vk::image_format("r16f")]] [[vk::binding(0, 0)]] RWTexture3D<float> dist_vol : register(u0, space0);
[[vk::image_format("rgba8")]] [[vk::binding(1, 0)]] RWTexture3D<float4> albedo_vol : register(u1, space0);
[[vk::image_format("rgba8")]] [[vk::binding(2, 0)]] RWTexture3D<float4> emissive_vol : register(u2, space0);

struct ClearPush {
  uint4 dims;    // x res, y clip index, zw unused
  float4 params; // x far distance
};
PUSH_CONSTANTS(ClearPush, pc);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
  uint res = pc.dims.x;
  if (any(id >= res)) return;
  uint3 texel = uint3(id.x, id.y, pc.dims.y * res + id.z);
  dist_vol[texel] = pc.params.x;
  albedo_vol[texel] = float4(0, 0, 0, 0);
  emissive_vol[texel] = float4(0, 0, 0, 0);
}
