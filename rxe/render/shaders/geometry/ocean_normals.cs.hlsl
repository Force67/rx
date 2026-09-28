#include "rhi_bindings.hlsli"
// Normal + foam from the finished displacement map: central differences of
// the displaced positions give the surface basis; the horizontal-displacement
// jacobian pinches below 1 exactly where chop folds the surface, driving the
// whitecap foam channel.
struct NormalsPush {
  uint size;
  float texel_world;  // patch_size / size
  float foam_scale;
  float pad0;
};
PUSH_CONSTANTS(NormalsPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> displacement : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> normal_foam : register(u1, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size || id.y >= pc.size) return;
  uint n = pc.size - 1;  // wrap masks (size is a power of two)
  uint xm = (id.x + n) & n, xp = (id.x + 1) & n;
  uint ym = (id.y + n) & n, yp = (id.y + 1) & n;

  float3 dxm = displacement[uint2(xm, id.y)].xyz;
  float3 dxp = displacement[uint2(xp, id.y)].xyz;
  float3 dym = displacement[uint2(id.x, ym)].xyz;
  float3 dyp = displacement[uint2(id.x, yp)].xyz;

  float step = 2.0 * pc.texel_world;
  float3 tangent_x = float3(step, 0.0, 0.0) + (dxp - dxm);
  float3 tangent_z = float3(0.0, 0.0, step) + (dyp - dym);
  float3 normal = normalize(cross(tangent_z, tangent_x));

  // Jacobian of the horizontal displacement (already includes choppiness).
  float jxx = 1.0 + (dxp.x - dxm.x) / step;
  float jzz = 1.0 + (dyp.z - dym.z) / step;
  float jxz = (dyp.x - dym.x) / step;
  float jzx = (dxp.z - dxm.z) / step;
  float jacobian = jxx * jzz - jxz * jzx;
  float foam = saturate((1.0 - jacobian) * pc.foam_scale);

  normal_foam[id.xy] = float4(normal, foam);
}
