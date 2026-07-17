#include "rhi_bindings.hlsli"
#include "placement_common.hlsli"
// GENERATE: discretizes one tile's density slab into an oriented point cloud
// by ordered dithering. One thread group per stack-tile, one thread per
// pattern point: the point's rank defines its threshold, the cumulative
// density intervals at its texel decide which layer (if any) it becomes.
// Survivors snap to the height map, derive a central-difference normal while
// the height texels are hot in cache, stage in groupshared memory and append
// to the global point buffer with a single atomic per group.

[[vk::binding(0, 0)]] StructuredBuffer<float2> pattern : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<float> density : register(t1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<PlacementPoint> points : register(u2, space0);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> point_count : register(u3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2DArray<float> world_maps : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState world_sampler : register(s4, space0);

struct PushData {
  float2 tile_origin;
  float tile_size;
  float footprint;
  float2 map_origin;
  float map_inv_extent;
  float map_meters_per_texel;  // normal derivation step
  float jitter;                // fraction of the footprint
  uint seed;
  uint stack_index;
  uint first_layer;
  uint layer_count;
  uint density_offset;
  uint height_map;
  int tile_x;
  int tile_z;
  uint point_capacity;
  uint pad0;
  uint pad1;
};
PUSH_CONSTANTS(PushData, push);

groupshared PlacementPoint gs_points[PLACEMENT_PATTERN_POINTS];
groupshared uint gs_count;
groupshared uint gs_base;

float SampleHeight(float2 world_pos) {
  float2 uv = (world_pos - push.map_origin) * push.map_inv_extent;
  return world_maps.SampleLevel(world_sampler, float3(uv, float(push.height_map)), 0.0);
}

[numthreads(PLACEMENT_PATTERN_POINTS, 1, 1)]
void main(uint3 id : SV_GroupThreadID) {
  uint i = id.x;
  if (i == 0) gs_count = 0;
  GroupMemoryBarrierWithGroupSync();

  // Jitter breaks up the repeated dither structure; a point that jitters off
  // the tile is dropped (its neighbour does not own it either), which keeps
  // every tile independently reproducible.
  uint jitter_seed =
      PlacementInstanceSeed(push.seed ^ 0x51AB71EDu, push.tile_x, push.tile_z, i,
                            push.stack_index);
  float jitter_x = (PlacementHashToUnit(jitter_seed) - 0.5) * push.jitter * push.footprint;
  float jitter_z = (PlacementHashToUnit(PlacementPcg(jitter_seed)) - 0.5) * push.jitter *
                   push.footprint;
  float local_x = pattern[i].x * push.tile_size + jitter_x;
  float local_z = pattern[i].y * push.tile_size + jitter_z;
  bool inside = local_x >= 0.0 && local_x < push.tile_size && local_z >= 0.0 &&
                local_z < push.tile_size;

  if (inside) {
    float threshold = (float(i) + 0.5) / float(PLACEMENT_PATTERN_POINTS);
    float texel_size = push.tile_size / float(PLACEMENT_DENSITY_RES);
    uint texel_x = min(uint(local_x / texel_size), PLACEMENT_DENSITY_RES - 1);
    uint texel_z = min(uint(local_z / texel_size), PLACEMENT_DENSITY_RES - 1);

    uint selected = PLACEMENT_MAX_STACK_LAYERS;
    for (uint layer = 0; layer < push.layer_count; ++layer) {
      float cumulative =
          density[push.density_offset +
                  (layer * PLACEMENT_DENSITY_RES + texel_z) * PLACEMENT_DENSITY_RES +
                  texel_x];
      if (threshold < cumulative) {
        selected = layer;
        break;
      }
    }

    if (selected != PLACEMENT_MAX_STACK_LAYERS) {
      float2 world_pos = push.tile_origin + float2(local_x, local_z);
      float height = SampleHeight(world_pos);
      float h = push.map_meters_per_texel;
      float dhx = SampleHeight(world_pos + float2(h, 0.0)) -
                  SampleHeight(world_pos - float2(h, 0.0));
      float dhz = SampleHeight(world_pos + float2(0.0, h)) -
                  SampleHeight(world_pos - float2(0.0, h));
      float3 normal = normalize(float3(-dhx, 2.0 * h, -dhz));

      PlacementPoint result;
      result.position = float3(world_pos.x, height, world_pos.y);
      result.pad0 = 0.0;
      result.normal = normal;
      result.layer = push.first_layer + selected;
      result.rank = i;
      result.tile_x = push.tile_x;
      result.tile_z = push.tile_z;
      result.pad1 = 0u;

      uint slot;
      InterlockedAdd(gs_count, 1u, slot);
      gs_points[slot] = result;
    }
  }

  GroupMemoryBarrierWithGroupSync();
  if (i == 0) {
    uint base;
    InterlockedAdd(point_count[0], gs_count, base);
    gs_base = base;
  }
  GroupMemoryBarrierWithGroupSync();

  if (i < gs_count) {
    uint dst = gs_base + i;
    if (dst < push.point_capacity) points[dst] = gs_points[i];
  }
}
