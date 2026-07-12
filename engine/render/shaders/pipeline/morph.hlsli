// Morph target (blend shape) accumulation shared by the mesh vertex shaders
// and the morph regression test. Deltas are 36-byte {position, normal,
// tangent} float3 triples laid out [target][vertex] (GpuMesh::morph_deltas);
// active weights are 8-byte (uint target, float weight) pairs (the frame
// morph weight buffer). Only nonzero-weight targets are in the pair list, so
// idle targets cost nothing.
//
// Dual path like the skinned bone palette: buffer device addresses on SPIR-V
// (vk::RawBufferLoad), ByteAddressBuffer root SRVs on DXIL (the d3d12 backend
// binds t997/t996 from the addresses at push bytes 160/168, see RHI.md).

#ifndef RX_MORPH_HLSLI_
#define RX_MORPH_HLSLI_

#ifdef __spirv__
void ApplyMorphs(uint64_t delta_address, uint64_t weight_address, uint first_weight,
                 uint weight_count, uint vertex_count, uint vertex_id,
                 inout float3 position, inout float3 normal, inout float3 tangent) {
  for (uint i = 0; i < weight_count; ++i) {
    uint2 pair = vk::RawBufferLoad<uint2>(weight_address + (first_weight + i) * 8);
    float weight = asfloat(pair.y);
    uint64_t base = delta_address + uint64_t(pair.x * vertex_count + vertex_id) * 36;
    position += weight * vk::RawBufferLoad<float3>(base);
    normal += weight * vk::RawBufferLoad<float3>(base + 12);
    tangent += weight * vk::RawBufferLoad<float3>(base + 24);
  }
}
#else
void ApplyMorphs(ByteAddressBuffer deltas, ByteAddressBuffer weights, uint first_weight,
                 uint weight_count, uint vertex_count, uint vertex_id,
                 inout float3 position, inout float3 normal, inout float3 tangent) {
  for (uint i = 0; i < weight_count; ++i) {
    uint2 pair = weights.Load2((first_weight + i) * 8);
    float weight = asfloat(pair.y);
    uint base = (pair.x * vertex_count + vertex_id) * 36;
    position += weight * asfloat(deltas.Load3(base));
    normal += weight * asfloat(deltas.Load3(base + 12));
    tangent += weight * asfloat(deltas.Load3(base + 24));
  }
}
#endif

#endif  // RX_MORPH_HLSLI_
