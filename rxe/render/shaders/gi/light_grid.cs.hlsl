#include "rhi_bindings.hlsli"
// Cascaded world-space light binning. One dispatch of 4x4x4 thread groups over
// 16^3 cells x 4 cascades. Phase 1: the group coarse-culls all lights against
// its own world AABB into a groupshared bit array (256 lights = 8 uints, sphere
// vs AABB). Phase 2: each thread tests the survivors against its cell AABB and
// writes the final id list. Spot/area lights are culled by their bounding
// sphere (pos_radius) in v1.

static const uint kCells = 16;
static const uint kCascades = 4;
static const uint kMaxLights = 256;
static const uint kMaxPerCell = 32;
static const uint kCellsPerCascade = kCells * kCells * kCells;

struct Light {
  float4 pos_radius;
  float4 color_intensity;
  float4 direction_type;
  float4 params;
};

struct GridParams {
  float4 cascade[kCascades];  // xyz snapped origin, w cell size
  uint4 info;                 // x cells/axis, y cascades, z max per cell, w unused
};

[[vk::binding(0, 0)]] StructuredBuffer<Light> lights : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> cell_counts : register(u1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> cell_ids : register(u2, space0);
[[vk::binding(3, 0)]] ConstantBuffer<GridParams> grid : register(b3, space0);

struct PushData {
  uint light_count;
  uint3 pad;
};
PUSH_CONSTANTS(PushData, push);

groupshared uint g_survivors[8];  // 256-bit coarse-cull mask for this group

bool SphereVsAabb(float3 c, float r, float3 mn, float3 mx) {
  float3 closest = clamp(c, mn, mx);
  float3 d = c - closest;
  return dot(d, d) <= r * r;
}

[numthreads(4, 4, 4)]
void main(uint3 gid : SV_GroupID, uint3 gt : SV_GroupThreadID, uint lane : SV_GroupIndex) {
  // Cascade split off the group's z: 16 cells / 4 = 4 groups per axis, so
  // group.z in [0, 4*kCascades) selects both cascade and the z group.
  uint cascade = gid.z / (kCells / 4u);
  uint groupz = gid.z % (kCells / 4u);
  float cell_size = grid.cascade[cascade].w;
  float3 origin = grid.cascade[cascade].xyz;

  // Group world AABB (covers a 4x4x4 block of cells).
  uint3 group_base = uint3(gid.x * 4u, gid.y * 4u, groupz * 4u);
  float3 gmin = origin + float3(group_base) * cell_size;
  float3 gmax = gmin + 4.0 * cell_size;

  // Phase 1: coarse-cull every light against the group AABB into the mask.
  if (lane < 8u) g_survivors[lane] = 0u;
  GroupMemoryBarrierWithGroupSync();
  for (uint li = lane; li < push.light_count && li < kMaxLights; li += 64u) {
    Light l = lights[li];
    if (SphereVsAabb(l.pos_radius.xyz, l.pos_radius.w, gmin, gmax)) {
      InterlockedOr(g_survivors[li >> 5u], 1u << (li & 31u));
    }
  }
  GroupMemoryBarrierWithGroupSync();

  // Phase 2: per-thread cell test over the survivors.
  uint3 cell = group_base + gt;
  float3 cmin = origin + float3(cell) * cell_size;
  float3 cmax = cmin + cell_size;
  uint flat = cascade * kCellsPerCascade + (cell.z * kCells + cell.y) * kCells + cell.x;

  uint count = 0u;
  [loop]
  for (uint word = 0u; word < 8u && count < kMaxPerCell; ++word) {
    uint bits = g_survivors[word];
    while (bits != 0u && count < kMaxPerCell) {
      uint bit = firstbitlow(bits);
      bits &= ~(1u << bit);
      uint idx = (word << 5u) + bit;
      Light l = lights[idx];
      if (SphereVsAabb(l.pos_radius.xyz, l.pos_radius.w, cmin, cmax)) {
        cell_ids[flat * kMaxPerCell + count] = idx;
        ++count;
      }
    }
  }
  cell_counts[flat] = count;
}
