// Regression-test wrapper around the shared morph accumulation
// (engine/render/shaders/pipeline/morph.hlsli): applies the pushed weight
// pairs to every vertex of a base {position, normal, tangent} stream, exactly
// what mesh.vs does before skinning. SPIR-V only, like the other
// buffer-device-address users (RX_SHADER_NO_DXIL).
#include "rhi_bindings.hlsli"
#include "morph.hlsli"

struct PushData {
  uint64_t delta_address;   // packed [target][vertex] morph deltas
  uint64_t weight_address;  // (uint target, float weight) pairs
  uint weight_count;
  uint vertex_count;
};
PUSH_CONSTANTS(PushData, push);

// 36-byte {position, normal, tangent} float3 triples, the delta layout.
[[vk::binding(0, 0)]] ByteAddressBuffer base_vertices : register(t0, space0);
[[vk::binding(1, 0)]] RWByteAddressBuffer out_vertices : register(u1, space0);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= push.vertex_count) return;
  uint offset = id.x * 36;
  float3 position = asfloat(base_vertices.Load3(offset));
  float3 normal = asfloat(base_vertices.Load3(offset + 12));
  float3 tangent = asfloat(base_vertices.Load3(offset + 24));
#ifdef __spirv__
  ApplyMorphs(push.delta_address, push.weight_address, 0, push.weight_count, push.vertex_count,
              id.x, position, normal, tangent);
#endif
  out_vertices.Store3(offset, asuint(position));
  out_vertices.Store3(offset + 12, asuint(normal));
  out_vertices.Store3(offset + 24, asuint(tangent));
}
