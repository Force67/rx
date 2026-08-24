#include "rhi_bindings.hlsli"
// Compute skinning for ray tracing. One thread per vertex: read the bind-pose
// vertex plus its bone influences, blend the palette matrices exactly as
// SkinVertex does in mesh.vs.hlsl, and write the posed vertex into a per-actor
// buffer in the SAME 52-byte asset::Vertex layout. That layout is what the hit
// shaders fetch through the bindless geometry table (rt_geometry.hlsli), so
// reflections, path tracing and alpha-tested hair need no shader change to see
// the animated pose.
//
// Unlike the vertex-stage skinning this reads everything through plain
// descriptors rather than device addresses: a compute pass has no root-SRV
// fallback to arrange, and the palette is already a storage buffer.
//
// asset::Vertex: position float3 (0), normal float3 (12), tangent float4 (24),
// uv float2 (40), color uint (48). asset::SkinnedVertexExtra: 4 u8 bone
// indices then 4 u8 unorm weights.
[[vk::binding(0, 0)]] ByteAddressBuffer base_vertices : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer skin_stream : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer bone_palette : register(t2, space0);
[[vk::binding(3, 0)]] RWByteAddressBuffer posed_vertices : register(u3, space0);

struct PushData {
  uint vertex_count;
  uint skin_offset;  // this actor's first bone in the frame palette
};
PUSH_CONSTANTS(PushData, push);

static const uint kVertexStride = 52;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const uint v = id.x;
  if (v >= push.vertex_count) return;

  const uint base = v * kVertexStride;
  const float3 in_position = asfloat(base_vertices.Load3(base));
  const float3 in_normal = asfloat(base_vertices.Load3(base + 12));
  const float4 in_tangent = asfloat(base_vertices.Load4(base + 24));

  const uint2 skin = skin_stream.Load2(v * 8);
  float3 position = float3(0, 0, 0);
  float3 normal = float3(0, 0, 0);
  float3 tangent = float3(0, 0, 0);
  // Each bone is a column-major 4x4 (64 bytes), so M*v is the weighted sum of
  // columns and normals/tangents ride the upper 3x3. Blended without
  // renormalizing, like the vertex stage: the shrunken normal length is the
  // skin-tension signal the shading path reads.
  [unroll]
  for (uint i = 0; i < 4; ++i) {
    const float w = float((skin.y >> (i * 8)) & 0xffu) / 255.0;
    if (w <= 0.0) continue;
    const uint bone = (skin.x >> (i * 8)) & 0xffu;
    const uint offset = (push.skin_offset + bone) * 64;
    const float4 c0 = asfloat(bone_palette.Load4(offset + 0));
    const float4 c1 = asfloat(bone_palette.Load4(offset + 16));
    const float4 c2 = asfloat(bone_palette.Load4(offset + 32));
    const float4 c3 = asfloat(bone_palette.Load4(offset + 48));
    position += w * (c0 * in_position.x + c1 * in_position.y + c2 * in_position.z + c3).xyz;
    normal += w * (c0.xyz * in_normal.x + c1.xyz * in_normal.y + c2.xyz * in_normal.z);
    tangent += w * (c0.xyz * in_tangent.x + c1.xyz * in_tangent.y + c2.xyz * in_tangent.z);
  }
  // No fallback for an unweighted vertex: it collapses to the model origin,
  // exactly as SkinVertex leaves it. Substituting the bind pose here would make
  // the ray-traced silhouette disagree with the rasterized one, which is a
  // harder bug to see than the spike a zero-weight vertex already draws.

  posed_vertices.Store3(base, asuint(position));
  posed_vertices.Store3(base + 12, asuint(normal));
  posed_vertices.Store4(base + 24, asuint(float4(tangent, in_tangent.w)));
  // uv and the packed colour are pose-invariant, but the buffer is never
  // otherwise written, so they have to be copied through here.
  posed_vertices.Store2(base + 40, base_vertices.Load2(base + 40));
  posed_vertices.Store(base + 48, base_vertices.Load(base + 48));
}
