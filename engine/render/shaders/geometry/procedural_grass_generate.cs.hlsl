#include "rhi_bindings.hlsli"

// One pipeline serves reset, ring/surface generation and indirect-finalize
// phases. Dispatch boundaries order near-to-far capacity allocation.
struct PushData {
  column_major float4x4 view_proj;
  float4 field_origin_extent;  // origin xz, extent xz
  uint4 field;                 // width, height, type count, seed
  float4 camera_stream;        // camera xyz, active radius
  float4 placement;            // spacing, ring inner/outer, refinement start
  int4 grid;                   // absolute min fine cell xz, coarse grid size xz
  uint4 counts;                // candidates, surfaces, logical cap, arena cap
  float4 density_lod;          // start, end, far density, minimum up normal
  float4 geometry_fade;        // geometry start/end, fade start/end
  uint4 control;               // phase, lattice stride, next stride
  float4 bend_field;           // min-corner xz, height origin, inverse extent
};
PUSH_CONSTANTS(PushData, push);

[[vk::binding(0, 0)]] ByteAddressBuffer field_data : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer type_data : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer surface_data : register(t2, space0);
[[vk::binding(3, 0)]] RWByteAddressBuffer instances : register(u3, space0);
[[vk::binding(4, 0)]] RWByteAddressBuffer draw_args : register(u4, space0);
[[vk::binding(5, 0)]] RWByteAddressBuffer counters : register(u5, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]]
Texture2D<float4> bend_history : register(t6, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]]
SamplerState bend_history_sampler : register(s6, space0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
Texture2D<float4> bend_metadata : register(t7, space0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]]
SamplerState bend_metadata_sampler : register(s7, space0);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]]
Texture2D<float2> bend_confidence : register(t8, space0);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]]
SamplerState bend_confidence_sampler : register(s8, space0);

struct GrassTypeData {
  float4 base_color;
  float4 tip_color;
  float4 dimensions;
  float4 shape;
  float4 material;
};

struct PackedGrassInstance {
  uint4 block0;
  uint4 block1;
  uint4 block2;
  uint4 block3;
  uint2 block4;
};

groupshared PackedGrassInstance staged_instances[64];
groupshared uint staged_far_lod[64];
groupshared uint staged_rank[64];
groupshared uint live_count;
groupshared uint accepted_count;
groupshared uint near_count;
groupshared uint far_count;
groupshared uint near_base;
groupshared uint far_base;
groupshared uint capacity_open;

uint Hash(uint x) {
  x ^= x >> 16u;
  x *= 0x7feb352du;
  x ^= x >> 15u;
  x *= 0x846ca68bu;
  x ^= x >> 16u;
  return x;
}

uint HashCell(int2 p, uint seed) {
  return Hash(asuint(p.x) * 0x9e3779b9u ^ asuint(p.y) * 0x85ebca6bu ^ seed);
}

float Random01(uint value) { return float(Hash(value) >> 8u) * (1.0 / 16777216.0); }

uint PackHalf2(float2 value) {
  return f32tof16(value.x) | (f32tof16(value.y) << 16u);
}

float2 Random2(int2 p, uint seed) {
  uint h = HashCell(p, seed);
  return float2(Random01(h), Random01(h ^ 0x68bc21ebu));
}

GrassTypeData LoadType(uint type_index) {
  uint address = type_index * 80u;
  GrassTypeData type;
  type.base_color = asfloat(type_data.Load4(address + 0u));
  type.tip_color = asfloat(type_data.Load4(address + 16u));
  type.dimensions = asfloat(type_data.Load4(address + 32u));
  type.shape = asfloat(type_data.Load4(address + 48u));
  type.material = asfloat(type_data.Load4(address + 64u));
  return type;
}

uint4 LoadField(int2 p) {
  p = clamp(p, int2(0, 0), int2(push.field.xy) - 1);
  return field_data.Load4((uint(p.y) * push.field.x + uint(p.x)) * 16u);
}

bool SampleHeightfield(float2 world_xz, uint seed, out float3 position,
                       out float3 normal, out float density, out float growth,
                       out uint type_index) {
  position = 0.0;
  normal = float3(0.0, 1.0, 0.0);
  density = 0.0;
  growth = 0.0;
  type_index = 0u;
  float2 uv = (world_xz - push.field_origin_extent.xy) /
              max(push.field_origin_extent.zw, float2(1e-4, 1e-4));
  if (any(uv < 0.0) || any(uv > 1.0)) return false;
  float2 texel = uv * float2(push.field.xy - 1u);
  int2 p = int2(floor(texel));
  float2 f = frac(texel);
  uint4 s00 = LoadField(p);
  uint4 s10 = LoadField(p + int2(1, 0));
  uint4 s01 = LoadField(p + int2(0, 1));
  uint4 s11 = LoadField(p + int2(1, 1));
  float4 weights = float4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y),
                          (1.0 - f.x) * f.y, f.x * f.y);
  float4 heights = asfloat(uint4(s00.x, s10.x, s01.x, s11.x));
  float4 densities = asfloat(uint4(s00.y, s10.y, s01.y, s11.y));
  float4 growths = asfloat(uint4(s00.w, s10.w, s01.w, s11.w));
  position = float3(world_xz.x, dot(heights, weights), world_xz.y);
  density = saturate(dot(densities, weights));
  growth = max(dot(growths, weights), 0.0);

  // Stochastic interpolation keeps authored categories discrete while making
  // low-resolution type-map boundaries biological instead of ruler-straight.
  float choose = Random01(seed ^ 0xb5297a4du);
  if (choose < weights.x) type_index = s00.z;
  else if (choose < weights.x + weights.y) type_index = s10.z;
  else if (choose < weights.x + weights.y + weights.z) type_index = s01.z;
  else type_index = s11.z;
  type_index = min(type_index, push.field.z - 1u);

  float2 meters_per_texel = push.field_origin_extent.zw /
                            max(float2(push.field.xy - 1u), float2(1.0, 1.0));
  float dhdx = lerp(heights.y - heights.x, heights.w - heights.z, f.y) /
               max(meters_per_texel.x, 1e-4);
  float dhdz = lerp(heights.z - heights.x, heights.w - heights.y, f.x) /
               max(meters_per_texel.y, 1e-4);
  normal = normalize(float3(-dhdx, 1.0, -dhdz));
  return all(isfinite(position)) && all(isfinite(normal)) &&
         isfinite(density) && isfinite(growth);
}

bool SampleSurface(uint surface_candidate, uint seed, out float3 position,
                   out float3 normal, out float density, out float growth,
                   out uint type_index, out uint stable_seed) {
  position = 0.0;
  normal = float3(0.0, 1.0, 0.0);
  density = 0.0;
  growth = 0.0;
  type_index = 0u;
  stable_seed = 0u;
  uint triangle_index = 0xffffffffu;
  uint local_candidate = 0u;
  uint4 meta = 0u;
  uint lo = 0u;
  uint hi = push.counts.y;
  [loop]
  for (uint step = 0u; step < 12u && lo < hi; ++step) {
    uint mid = lo + (hi - lo) / 2u;
    uint4 candidate_meta = surface_data.Load4(mid * 64u + 48u);
    if (surface_candidate < candidate_meta.x) {
      hi = mid;
    } else if (surface_candidate >= candidate_meta.x + candidate_meta.y) {
      lo = mid + 1u;
    } else {
      triangle_index = mid;
      meta = candidate_meta;
      local_candidate = surface_candidate - meta.x;
      break;
    }
  }
  if (triangle_index == 0xffffffffu) return false;
  uint address = triangle_index * 64u;
  float4 p0 = asfloat(surface_data.Load4(address + 0u));
  float4 p1 = asfloat(surface_data.Load4(address + 16u));
  float4 p2 = asfloat(surface_data.Load4(address + 32u));
  stable_seed = Hash(meta.z ^ local_candidate * 0x9e3779b9u ^ seed);
  float r0 = Random01(stable_seed);
  float r1 = Random01(stable_seed ^ 0x68bc21ebu);
  float sr0 = sqrt(r0);
  float3 bary = float3(1.0 - sr0, sr0 * (1.0 - r1), sr0 * r1);
  position = p0.xyz * bary.x + p1.xyz * bary.y + p2.xyz * bary.z;
  normal = normalize(cross(p1.xyz - p0.xyz, p2.xyz - p0.xyz));
  density = saturate(p0.w);
  growth = max(p1.w, 0.0);
  type_index = min(asuint(p2.w), push.field.z - 1u);
  return all(isfinite(position)) && all(isfinite(normal)) &&
         isfinite(density) && isfinite(growth);
}

void FindClump(float2 world_xz, float scale, uint seed, out float2 feature,
               out uint clump_seed) {
  float2 grid = world_xz / max(scale, 0.2);
  int2 cell = int2(floor(grid));
  float best = 1e20;
  feature = grid;
  clump_seed = seed;
  [unroll]
  for (int y = -1; y <= 1; ++y) {
    [unroll]
    for (int x = -1; x <= 1; ++x) {
      int2 candidate_cell = cell + int2(x, y);
      float2 candidate = float2(candidate_cell) + Random2(candidate_cell, seed);
      float d2 = dot(candidate - grid, candidate - grid);
      if (d2 < best) {
        best = d2;
        feature = candidate * scale;
        clump_seed = HashCell(candidate_cell, seed);
      }
    }
  }
}

bool Generate(uint candidate, out PackedGrassInstance packed, out uint far_lod) {
  packed = (PackedGrassInstance)0;
  far_lod = 0u;
  float3 position, normal;
  float density, growth;
  uint type_index;
  uint stable_seed = Hash(candidate ^ push.field.w);
  if (push.control.x == 1u) {
    uint gx = candidate % uint(push.grid.z);
    uint gz = candidate / uint(push.grid.z);
    int2 cell = push.grid.xy + int2(gx, gz) * int(push.control.y);
    stable_seed = HashCell(cell, push.field.w);
    float2 jitter = (float2(Random01(stable_seed),
                            Random01(stable_seed ^ 0x68bc21ebu)) - 0.5) * 0.86;
    float2 world_xz = (float2(cell) + 0.5 + jitter) * push.placement.x;
    float ring_distance = distance(world_xz, push.camera_stream.xz);
    if (ring_distance < push.placement.y || ring_distance >= push.placement.z) return false;
    if (push.control.z > push.control.y && ring_distance >= push.placement.w) {
      int next_stride = int(push.control.z);
      if (cell.x % next_stride == 0 && cell.y % next_stride == 0) return false;
      float refinement = 1.0 - smoothstep(push.placement.w, push.placement.z,
                                          ring_distance);
      if (Random01(stable_seed ^ 0x42f0e1ebu) > refinement) return false;
    }
    if (!SampleHeightfield(world_xz, stable_seed, position, normal, density,
                           growth, type_index)) return false;
  } else if (push.control.x == 2u) {
    if (!SampleSurface(candidate, push.field.w, position, normal,
                       density, growth, type_index, stable_seed)) return false;
  } else return false;

  if (growth <= 0.0 || density <= 0.0 || normal.y < push.density_lod.w) return false;
  float distance_to_eye = distance(position, push.camera_stream.xyz);
  if (distance_to_eye > min(push.camera_stream.w, push.geometry_fade.w)) return false;

  float density_lod = smoothstep(push.density_lod.x, push.density_lod.y,
                                 distance_to_eye);
  density *= lerp(1.0, push.density_lod.z, density_lod);
  if (Random01(stable_seed ^ 0x1b56c4e9u) > density) return false;

  // Conservative point-frustum rejection. The margin retains tall blades whose
  // roots are just outside an edge; exact shape expansion happens in the VS.
  float4 clip = mul(push.view_proj, float4(position, 1.0));
  float margin = max(clip.w * 0.08, 0.2);
  if (clip.w <= 0.0 || abs(clip.x) > clip.w + margin ||
      abs(clip.y) > clip.w + margin || clip.z < -margin || clip.z > clip.w + margin) return false;

  GrassTypeData type = LoadType(type_index);
  float2 clump_feature;
  uint clump_seed;
  FindClump(position.xz, type.material.z, push.field.w, clump_feature, clump_seed);
  float clump_angle = Random01(clump_seed) * 6.28318530718;
  float blade_angle = clump_angle +
                      (Random01(stable_seed ^ 0x94d049bbu) - 0.5) * 0.9;
  float2 facing = float2(cos(blade_angle), sin(blade_angle));
  float height = lerp(type.dimensions.x, type.dimensions.y,
                      Random01(stable_seed ^ 0x369dea0fu)) * growth;
  float width = lerp(type.dimensions.z, type.dimensions.w,
                     Random01(stable_seed ^ 0xdb4f0b91u));
  float tilt = lerp(type.shape.x, type.shape.y,
                    Random01(stable_seed ^ 0xbb67ae85u));
  float bend = type.shape.z * lerp(0.65, 1.35,
                                   Random01(stable_seed ^ 0x3c6ef372u));
  float side = type.shape.w * (Random01(stable_seed ^ 0xa54ff53au) * 2.0 - 1.0);
  float tint = 1.0 + (Random01(clump_seed ^ 0x510e527fu) * 2.0 - 1.0) *
                          type.base_color.a;
  if (!all(isfinite(float4(height, width, tilt, bend))) ||
      !all(isfinite(float4(side, tint, type.material.y, type.material.z)))) return false;
  float geometry_lod = smoothstep(push.geometry_fade.x, push.geometry_fade.y,
                                  distance_to_eye);
  float visibility = 1.0 - smoothstep(push.geometry_fade.z,
                                      push.geometry_fade.w, distance_to_eye);
  height *= visibility;
  width *= max(visibility, 0.12);
  if (height <= 0.01) return false;

  far_lod = Random01(stable_seed ^ 0x6c8e9cf5u) < geometry_lod ? 1u : 0u;
  float4 bend_history_value = 0.0;
  float2 bend_uv = (position.xz - push.bend_field.xy) * push.bend_field.w;
  if (push.bend_field.w > 0.0 &&
      all(bend_uv >= 0.0) && all(bend_uv <= 1.0)) {
    bend_history_value = bend_history.SampleLevel(bend_history_sampler, bend_uv, 0.0);
    float4 metadata = bend_metadata.SampleLevel(bend_metadata_sampler, bend_uv, 0.0);
    float2 confidence =
        bend_confidence.SampleLevel(bend_confidence_sampler, bend_uv, 0.0);
    if (!all(isfinite(bend_history_value)) || !all(isfinite(metadata)) ||
        !all(isfinite(confidence))) {
      bend_history_value = 0.0;
    } else {
      float current_confidence = confidence.x;
      float previous_confidence = confidence.y;
      if (current_confidence > 1e-4) {
        float current_height =
            push.bend_field.z + metadata.x / current_confidence;
        float current_radius = metadata.y / current_confidence;
        float current_vertical = 1.0 - smoothstep(
            current_radius * 0.15, max(current_radius, 0.01),
            abs(position.y - current_height));
        bend_history_value.xy *= current_vertical;
      } else bend_history_value.xy = 0.0;
      if (previous_confidence > 1e-4) {
        float previous_height =
            push.bend_field.z + metadata.z / previous_confidence;
        float previous_radius = metadata.w / previous_confidence;
        float previous_vertical = 1.0 - smoothstep(
            previous_radius * 0.15, max(previous_radius, 0.01),
            abs(position.y - previous_height));
        bend_history_value.zw *= previous_vertical;
      } else bend_history_value.zw = 0.0;
    }
  }
  packed.block0 = asuint(float4(position, height));
  packed.block1 = asuint(float4(facing, width, tint));
  packed.block2 = uint4(asuint(normal.x), asuint(normal.y), asuint(normal.z), type_index);
  packed.block3 = asuint(float4(tilt, bend, side, geometry_lod));
  packed.block4 = uint2(PackHalf2(bend_history_value.xy),
                        PackHalf2(bend_history_value.zw));
  return true;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID, uint group_index : SV_GroupIndex) {
  if (push.control.x == 0u) {
    if (dispatch_id.x == 0u) {
      counters.Store4(0u, uint4(0u, 0u, 0u, 0u));
      counters.Store4(16u, uint4(0u, 0u, 0u, 0u));
      draw_args.Store4(0u, uint4(0u, 0u, 0u, 0u));
      draw_args.Store4(16u, uint4(0u, 0u, 0u, 0u));
    }
    return;
  }
  if (push.control.x == 3u) {
    if (dispatch_id.x == 0u) {
      uint4 count = counters.Load4(0u);
      uint near_blades = min(count.y, push.counts.z);
      uint far_blades = min(count.z, push.counts.z - near_blades);
      draw_args.Store4(0u, uint4(42u, near_blades, 0u, 0u));
      draw_args.Store4(16u, uint4(18u, far_blades, 0u, 0u));
      counters.Store(16u, near_blades > 0u ? 1u : 0u);
      counters.Store(20u, far_blades > 0u ? 1u : 0u);
    }
    return;
  }

  if (group_index == 0u) {
    live_count = 0u;
    uint reserved;
    counters.InterlockedAdd(0u, 0u, reserved);
    capacity_open = reserved < push.counts.z ? 1u : 0u;
  }
  GroupMemoryBarrierWithGroupSync();
  if (capacity_open == 0u) return;

  PackedGrassInstance packed;
  uint far_lod;
  bool live = dispatch_id.x < push.counts.x &&
              Generate(dispatch_id.x, packed, far_lod);
  if (live) {
    uint local_slot;
    InterlockedAdd(live_count, 1u, local_slot);
    staged_instances[local_slot] = packed;
    staged_far_lod[local_slot] = far_lod;
  }
  GroupMemoryBarrierWithGroupSync();

  if (group_index == 0u) {
    uint total_base;
    counters.InterlockedAdd(0u, live_count, total_base);
    accepted_count = total_base < push.counts.z
                         ? min(live_count, push.counts.z - total_base)
                         : 0u;
    near_count = 0u;
    far_count = 0u;
  }
  GroupMemoryBarrierWithGroupSync();

  if (group_index < accepted_count) {
    uint rank;
    if (staged_far_lod[group_index] != 0u) InterlockedAdd(far_count, 1u, rank);
    else InterlockedAdd(near_count, 1u, rank);
    staged_rank[group_index] = rank;
  }
  GroupMemoryBarrierWithGroupSync();

  if (group_index == 0u) {
    counters.InterlockedAdd(4u, near_count, near_base);
    counters.InterlockedAdd(8u, far_count, far_base);
  }
  GroupMemoryBarrierWithGroupSync();

  if (group_index < accepted_count) {
    PackedGrassInstance instance = staged_instances[group_index];
    uint slot = staged_far_lod[group_index] != 0u
                    ? push.counts.w - 1u - (far_base + staged_rank[group_index])
                    : near_base + staged_rank[group_index];
    uint address = slot * 72u;
    instances.Store4(address + 0u, instance.block0);
    instances.Store4(address + 16u, instance.block1);
    instances.Store4(address + 32u, instance.block2);
    instances.Store4(address + 48u, instance.block3);
    instances.Store2(address + 64u, instance.block4);
  }
}
