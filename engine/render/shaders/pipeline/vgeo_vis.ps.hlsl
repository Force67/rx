#include "rhi_bindings.hlsli"
#include "vgeo_common.hlsli"
// Pixel half of the hardware visibility raster: packs SV_Position depth with
// the per-primitive id and resolves against the compute rasterizer's output
// with the same 64-bit InterlockedMax. No render targets are bound; the
// visibility buffer is the only side effect.

[[vk::binding(0, 0)]] StructuredBuffer<VgeoParams> params_buf : register(t0, space0);
[[vk::binding(8, 0)]] RWStructuredBuffer<uint64_t> visbuffer : register(u8, space0);

struct PsIn {
  float4 pos : SV_Position;
  uint id : SV_PrimitiveID;
};

void main(PsIn input) {
  VgeoParams p = params_buf[0];
  uint2 px = uint2(input.pos.xy);
  uint64_t value = (uint64_t(asuint(input.pos.z)) << 32) | uint64_t(input.id);
  InterlockedMax(visbuffer[px.y * p.width + px.x], value);
}
