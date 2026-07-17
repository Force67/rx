#include "rhi_bindings.hlsli"
#include "placement_common.hlsli"
// PLACEMENT: expands every oriented point into a final world transform in the
// host-visible result buffer. The append order of the point buffer is not
// stable across runs, so all randomness is seeded from the point's positional
// identity (tile + pattern rank + layer) - regenerating a tile reproduces the
// same instances bit for bit regardless of GPU scheduling.

[[vk::binding(0, 0)]] StructuredBuffer<PlacementPoint> points : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<PlacementLayerGpu> layers : register(t1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<PlacementInstance> instances : register(u2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint> point_count : register(t3, space0);

struct PushData {
  uint seed;
  uint point_capacity;
  uint pad0;
  uint pad1;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint i = id.x;
  uint count = min(point_count[0], push.point_capacity);
  if (i >= count) return;

  PlacementPoint pt = points[i];
  PlacementLayerGpu desc = layers[pt.layer];

  uint seed = PlacementInstanceSeed(push.seed, pt.tile_x, pt.tile_z, pt.rank, pt.layer);
  float yaw = (desc.flags & 1u) != 0u ? PlacementHashToUnit(seed) * 6.28318530 : 0.0;
  float scale = desc.scale_min +
                (desc.scale_max - desc.scale_min) * PlacementHashToUnit(PlacementPcg(seed));

  // Blend the up axis toward the surface normal by the layer's tilt, then
  // yaw the tangent frame around it. Mirrors BuildPlacementTransform.
  float3 up = float3(0.0, 1.0, 0.0);
  float3 axis = up + (pt.normal - up) * desc.tilt;
  float len = length(axis);
  axis = len < 1e-5 ? up : axis / len;
  float c = cos(yaw);
  float s = sin(yaw);
  float3 ref = abs(axis.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
  float3 tangent = normalize(cross(ref, axis));
  // tangent x axis keeps the frame right-handed (mirrored transforms are
  // rejected by the instance store).
  float3 bitangent = cross(tangent, axis);
  float3 basis_x = tangent * c + bitangent * s;
  float3 basis_z = -tangent * s + bitangent * c;

  // column_major float4x4: columns are the scaled basis vectors + translation.
  float4x4 m;
  m[0] = float4(basis_x * scale, 0.0);
  m[1] = float4(axis * scale, 0.0);
  m[2] = float4(basis_z * scale, 0.0);
  m[3] = float4(pt.position + float3(0.0, desc.y_offset, 0.0), 1.0);

  PlacementInstance instance;
  instance.transform = transpose(m);
  instance.layer = pt.layer;
  instance.rank = pt.rank;
  instance.tile_x = pt.tile_x;
  instance.tile_z = pt.tile_z;
  instances[i] = instance;
}
