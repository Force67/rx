#include "rhi_bindings.hlsli"
// Assembles the world-space displacement map from the two IFFT'd spectra:
// (lambda*dx, h, lambda*dz), with the (-1)^(x+z) centering sign and the
// 1/N^2 inverse-transform normalization.
struct FinalizePush {
  uint size;
  float choppiness;
  float amplitude;
  float pad0;
};
PUSH_CONSTANTS(FinalizePush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> spec_hx : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> spec_z : register(u1, space0);
[[vk::binding(2, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> displacement : register(u2, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size || id.y >= pc.size) return;
  float sign = ((id.x + id.y) & 1u) != 0 ? -1.0 : 1.0;
  float norm = sign * pc.amplitude / (float(pc.size) * float(pc.size));
  float4 hx = spec_hx[id.xy];
  float2 z = spec_z[id.xy].xy;
  float3 d = float3(hx.z * pc.choppiness, hx.x, z.x * pc.choppiness) * norm;
  displacement[id.xy] = float4(d, 0.0);
}
