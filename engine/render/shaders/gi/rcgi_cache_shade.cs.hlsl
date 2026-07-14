#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
#include "gi/light_grid.hlsli"
// RCGI cache shade. Indirect dispatch over the active-cell list the probe trace
// built (one thread per stale entry). Reconstructs the hit triangle/material
// through the bindless scene tables, lights it with sun (one shadow ray) +
// emissive + point/spot lights from the world light grid + a previous-frame
// bounce from the irradiance cascades, and stores the HDR radiance into the
// cache. Amortised: each unique world cell is shaded once, not once per ray.

[[vk::binding(0, 0)]] ConstantBuffer<RcgiGlobals> rcgi : register(b0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> rcgi_state_rw : register(u1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint2> rcgi_radiance_rw : register(u2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint> active_list : register(t3, space0);
[[vk::binding(4, 0)]] StructuredBuffer<uint> active_meta : register(t4, space0);
[[vk::binding(5, 0)]] RaytracingAccelerationStructure tlas : register(t5, space0);

struct Light {
  float4 pos_radius;
  float4 color_intensity;
  float4 direction_type;
  float4 params;
};
[[vk::binding(6, 0)]] StructuredBuffer<Light> lights : register(t6, space0);
[[vk::binding(7, 0)]] StructuredBuffer<uint> lg_counts : register(t7, space0);
[[vk::binding(8, 0)]] StructuredBuffer<uint> lg_ids : register(t8, space0);
[[vk::binding(9, 0)]] ConstantBuffer<LightGridParams> lg_grid : register(b9, space0);
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]] Texture2D rcgi_irr_atlas : register(t10, space0);
[[vk::combinedImageSampler]] [[vk::binding(10, 0)]] SamplerState rcgi_irr_sampler : register(s10, space0);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]] Texture2D rcgi_vis_atlas : register(t11, space0);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]] SamplerState rcgi_vis_sampler : register(s11, space0);

// Bindless scene tables (set 1), full material resolution.
#define RX_GEOMETRY_SPACE space1
#include "rt_geometry.hlsli"
struct MaterialRecord {
  float4 base_color_factor;
  float3 emissive;
  uint base_color_texture;
  uint flags;
  float alpha_cutoff;
  float roughness;
  float metallic;
  uint metallic_roughness_texture;
  uint pad0;
  uint pad1;
  uint pad2;
};
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D bindless_textures[RX_BINDLESS_TEXTURE_COUNT] : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState bindless_sampler : register(s4, space1);

float3 LightContribution(Light l, float3 pos, float3 n, float3 albedo) {
  float3 L = l.pos_radius.xyz - pos;
  float d2 = dot(L, L);
  float radius = l.pos_radius.w;
  if (d2 > radius * radius) return 0.0.xxx;
  float d = sqrt(max(d2, 1e-6));
  float3 l_dir = L / d;
  float ndl = max(dot(n, l_dir), 0.0);
  if (ndl <= 0.0) return 0.0.xxx;
  float win = saturate(1.0 - pow(d / radius, 4.0));
  win *= win;
  float atten = win / (d2 + 1.0);
  uint type = (uint)l.direction_type.w;
  if (type == 1u) {  // spot cone
    float cd = dot(normalize(l.direction_type.xyz), -l_dir);
    atten *= saturate((cd - l.params.y) / max(l.params.x - l.params.y, 1e-3));
  }
  return albedo / kRcgiPi * l.color_intensity.rgb * l.color_intensity.w * ndl * atten;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= active_meta[0]) return;
  uint idx = active_list[id.x];
  uint base = idx * kRcgiEntry;

  uint hit0 = rcgi_state_rw[base + kRcgiOffHit0];
  uint instance = hit0 & 0x00ffffffu;
  uint geom_index = hit0 >> 24u;
  uint prim = rcgi_state_rw[base + kRcgiOffHit1];
  uint packed_bary = rcgi_state_rw[base + kRcgiOffHit2];
  float2 bary = float2(f16tof32(packed_bary & 0xffffu), f16tof32(packed_bary >> 16u));
  float3 pos = asfloat(uint3(rcgi_state_rw[base + kRcgiOffPosX],
                             rcgi_state_rw[base + kRcgiOffPosY],
                             rcgi_state_rw[base + kRcgiOffPosZ]));
  float3 n = RcgiUnpackOct(rcgi_state_rw[base + kRcgiOffNrm]);

  // Defensive bounds check on the unpacked geometry indices. The payload is now
  // single-writer (probe trace claims one owner per cell), but a stale/evicted
  // slot or corrupt key could still surface an out-of-range instance; reject it
  // rather than issue an out-of-bounds bindless fetch.
  uint mesh_count, geom_count, mat_count, stride;
  mesh_records.GetDimensions(mesh_count, stride);
  geometry_records.GetDimensions(geom_count, stride);
  material_records.GetDimensions(mat_count, stride);
  if (instance >= mesh_count) return;

  // Material: interpolate uv, sample base colour, read emissive.
  MeshRecord mesh = mesh_records[NonUniformResourceIndex(instance)];
  uint geom_slot = mesh.geometry_offset + geom_index;
  if (geom_slot >= geom_count) return;
  GeometryRecord geometry = geometry_records[geom_slot];
  uint3 tri = RxLoadTriangle(mesh, geometry.index_offset + prim * 3);
  float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
  float2 uv = RxLoadUv(mesh, tri[0]) * w[0] + RxLoadUv(mesh, tri[1]) * w[1] +
              RxLoadUv(mesh, tri[2]) * w[2];
  if (geometry.material_index >= mat_count) return;
  MaterialRecord material = material_records[NonUniformResourceIndex(geometry.material_index)];
  float3 albedo = material.base_color_factor.rgb;
  if (material.base_color_texture != 0xffffffffu) {
    albedo *= bindless_textures[NonUniformResourceIndex(material.base_color_texture)]
                  .SampleLevel(bindless_sampler, uv, 3.0).rgb;
  }

  // Sun (one shadow ray).
  float3 to_sun = normalize(-rcgi.sun_direction.xyz);
  float ndl = max(dot(n, to_sun), 0.0);
  float shadow = 1.0;
  if (ndl > 0.0) {
    RayDesc sray;
    sray.Origin = pos + n * 0.02;
    sray.TMin = 0.001;
    sray.Direction = to_sun;
    sray.TMax = 1000.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> srq;
    srq.TraceRayInline(tlas, RAY_FLAG_NONE, RX_RAY_MASK_REALTIME, sray);
    srq.Proceed();
    if (srq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) shadow = 0.0;
  }
  float3 sun = rcgi.sun_color.rgb * rcgi.sun_direction.w;
  float3 radiance = albedo / kRcgiPi * sun * ndl * shadow + material.emissive;

  // Point/spot lights from the world light grid.
  uint flat_cell;
  if (LightGridCell(lg_grid, pos, flat_cell)) {
    uint count = lg_counts[flat_cell];
    for (uint i = 0u; i < count && i < RX_LG_MAX_PER_CELL; ++i) {
      radiance += LightContribution(lights[lg_ids[flat_cell * RX_LG_MAX_PER_CELL + i]],
                                    pos, n, albedo);
    }
  }

  // Previous-frame bounce from the irradiance cascades (multi-bounce).
  radiance += albedo * SampleRcgiIrradiance(rcgi, rcgi_irr_atlas, rcgi_irr_sampler, rcgi_vis_atlas,
                                            rcgi_vis_sampler, pos, n, n);

  rcgi_radiance_rw[idx] = RcgiPackRadiance(radiance);
  // +1 encoded (0 = never shaded) so RcgiCacheLookup can reject unshaded entries.
  rcgi_state_rw[base + kRcgiOffStamp] = RcgiStampEncode(rcgi.misc.y);
}
