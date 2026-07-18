#include "rhi_bindings.hlsli"

// One dispatch serves reset, candidate generation and indirect-finalize phases.
// Keeping them separate avoids cross-workgroup races on the append counter.
struct PushData {
  column_major float4x4 view_proj;
  float4 field_origin_extent;  // origin xz, extent xz
  uint4 field;                 // width, height, type count, seed
  float4 camera_stream;        // camera xyz, active radius
  float4 placement;            // candidate spacing, stream tile size
  int4 grid;                   // absolute min cell xz, active cell counts xz
  uint4 counts;                // terrain, total candidates, surfaces, blade cap
  float4 density_lod;          // start, end, far density, minimum up normal
  float4 geometry_fade;        // geometry start/end, fade start/end
  uint4 control;               // phase
};
PUSH_CONSTANTS(PushData, push);

[[vk::binding(0, 0)]] ByteAddressBuffer field_data : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer type_data : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer surface_data : register(t2, space0);
[[vk::binding(3, 0)]] RWByteAddressBuffer instances : register(u3, space0);
[[vk::binding(4, 0)]] RWByteAddressBuffer draw_args : register(u4, space0);
[[vk::binding(5, 0)]] RWByteAddressBuffer counters : register(u5, space0);

struct GrassTypeData {
  float4 base_color;
  float4 tip_color;
  float4 dimensions;
  float4 shape;
  float4 material;
};

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

float HeightAt(float2 world_xz) {
  float2 uv = (world_xz - push.field_origin_extent.xy) /
              max(push.field_origin_extent.zw, float2(1e-4, 1e-4));
  float2 texel = saturate(uv) * float2(push.field.xy - 1u);
  int2 p = int2(floor(texel));
  float2 f = frac(texel);
  float h00 = asfloat(LoadField(p).x);
  float h10 = asfloat(LoadField(p + int2(1, 0)).x);
  float h01 = asfloat(LoadField(p + int2(0, 1)).x);
  float h11 = asfloat(LoadField(p + int2(1, 1)).x);
  return lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
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
  float hx0 = HeightAt(world_xz - float2(meters_per_texel.x, 0.0));
  float hx1 = HeightAt(world_xz + float2(meters_per_texel.x, 0.0));
  float hz0 = HeightAt(world_xz - float2(0.0, meters_per_texel.y));
  float hz1 = HeightAt(world_xz + float2(0.0, meters_per_texel.y));
  normal = normalize(float3((hx0 - hx1) / max(meters_per_texel.x, 1e-4),
                            2.0,
                            (hz0 - hz1) / max(meters_per_texel.y, 1e-4)));
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
  [loop]
  for (uint i = 0u; i < push.counts.z; ++i) {
    meta = surface_data.Load4(i * 64u + 48u);
    if (surface_candidate >= meta.x && surface_candidate < meta.x + meta.y) {
      triangle_index = i;
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

void Generate(uint candidate) {
  float3 position, normal;
  float density, growth;
  uint type_index;
  uint stable_seed = Hash(candidate ^ push.field.w);
  if (candidate < push.counts.x) {
    uint gx = candidate % uint(push.grid.z);
    uint gz = candidate / uint(push.grid.z);
    int2 cell = push.grid.xy + int2(gx, gz);
    stable_seed = HashCell(cell, push.field.w);
    float2 jitter = (float2(Random01(stable_seed),
                            Random01(stable_seed ^ 0x68bc21ebu)) - 0.5) * 0.86;
    float2 world_xz = (float2(cell) + 0.5 + jitter) * push.placement.x;
    if (!SampleHeightfield(world_xz, stable_seed, position, normal, density,
                           growth, type_index)) return;
  } else {
    if (!SampleSurface(candidate - push.counts.x, push.field.w, position, normal,
                       density, growth, type_index, stable_seed)) return;
  }

  if (growth <= 0.0 || density <= 0.0 || normal.y < push.density_lod.w) return;
  float distance_to_eye = distance(position, push.camera_stream.xyz);
  if (distance_to_eye > min(push.camera_stream.w, push.geometry_fade.w)) return;

  float density_lod = smoothstep(push.density_lod.x, push.density_lod.y,
                                 distance_to_eye);
  density *= lerp(1.0, push.density_lod.z, density_lod);
  if (Random01(stable_seed ^ 0x1b56c4e9u) > density) return;

  // Conservative point-frustum rejection. The margin retains tall blades whose
  // roots are just outside an edge; exact shape expansion happens in the VS.
  float4 clip = mul(push.view_proj, float4(position, 1.0));
  float margin = max(clip.w * 0.08, 0.2);
  if (clip.w <= 0.0 || abs(clip.x) > clip.w + margin ||
      abs(clip.y) > clip.w + margin || clip.z < -margin || clip.z > clip.w + margin) return;

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
      !all(isfinite(float4(side, tint, type.material.y, type.material.z)))) return;
  float geometry_lod = smoothstep(push.geometry_fade.x, push.geometry_fade.y,
                                  distance_to_eye);
  float visibility = 1.0 - smoothstep(push.geometry_fade.z,
                                      push.geometry_fade.w, distance_to_eye);
  height *= visibility;
  width *= max(visibility, 0.12);
  if (height <= 0.01) return;

  uint slot;
  counters.InterlockedAdd(4u, 1u, slot);
  if (slot >= push.counts.w) return;
  uint address = slot * 64u;
  instances.Store4(address + 0u, asuint(float4(position, height)));
  instances.Store4(address + 16u, asuint(float4(facing, width, tint)));
  instances.Store4(address + 32u,
                   uint4(asuint(normal.x), asuint(normal.y), asuint(normal.z), type_index));
  instances.Store4(address + 48u, asuint(float4(tilt, bend, side, geometry_lod)));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  if (push.control.x == 0u) {
    if (dispatch_id.x == 0u) {
      counters.Store(0u, 0u);
      counters.Store(4u, 0u);
      draw_args.Store4(0u, uint4(0u, 0u, 0u, 0u));
    }
    return;
  }
  if (push.control.x == 1u) {
    if (dispatch_id.x < push.counts.y) Generate(dispatch_id.x);
    return;
  }
  if (dispatch_id.x == 0u) {
    uint blade_count = min(counters.Load(4u), push.counts.w);
    draw_args.Store4(0u, uint4(42u, blade_count, 0u, 0u));
    counters.Store(0u, blade_count > 0u ? 1u : 0u);
  }
}
