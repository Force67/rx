#include "rhi_bindings.hlsli"
#include "gi/sdf_trace.hlsli"
// Composes one instance's mesh SDF into one clip of the global SDF clipmap.
// Dispatched once per (clip being recomposed, overlapping instance); each voxel
// min-blends this instance's conservative distance and, when it wins, writes the
// instance's flat albedo/emissive into the colour proxies.
//
// The mesh SDF is a flat StructuredBuffer<float> (local-space signed distance),
// sampled with manual trilinear. Points outside the mesh volume use
// distance-to-box + the clamped edge value, a conservative underestimate that
// keeps the sphere-trace from overshooting and avoids a false shell at the box.
// Non-uniform instance scale is folded in by a single conservative world-scale
// factor (the transform's minimum axis scale -- see the .cc for the rationale).

[[vk::image_format("r16f")]] [[vk::binding(0, 0)]] RWTexture3D<float> dist_vol : register(u0, space0);
[[vk::image_format("rgba8")]] [[vk::binding(1, 0)]] RWTexture3D<float4> albedo_vol : register(u1, space0);
[[vk::image_format("rgba8")]] [[vk::binding(2, 0)]] RWTexture3D<float4> emissive_vol : register(u2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<float> mesh_sdf : register(t3, space0);

struct ComposePush {
  column_major float4x4 inv_transform;  // world -> mesh local
  float4 box_min;      // xyz mesh-local volume min corner, w mesh voxel size
  uint4 mesh_res;      // xyz mesh volume resolution, w clip index
  float4 clip_origin;  // xyz clip world min corner, w clip voxel size
  float4 albedo;       // rgb flat albedo
  float4 emissive;     // rgb flat emissive
  float4 misc;         // x conservative world-scale factor (local dist -> world dist)
};
PUSH_CONSTANTS(ComposePush, pc);

float FetchMesh(uint3 c) {
  c = min(c, pc.mesh_res.xyz - 1);
  uint idx = (c.z * pc.mesh_res.y + c.y) * pc.mesh_res.x + c.x;
  return mesh_sdf[idx];
}

// Trilinear sample of the mesh SDF at mesh-voxel coordinate mc (in [0,res]).
float SampleMesh(float3 mc) {
  float3 clamped = clamp(mc - 0.5, 0.0, float3(pc.mesh_res.xyz) - 1.0);
  uint3 b = (uint3)floor(clamped);
  float3 f = clamped - float3(b);
  float c000 = FetchMesh(b + uint3(0, 0, 0));
  float c100 = FetchMesh(b + uint3(1, 0, 0));
  float c010 = FetchMesh(b + uint3(0, 1, 0));
  float c110 = FetchMesh(b + uint3(1, 1, 0));
  float c001 = FetchMesh(b + uint3(0, 0, 1));
  float c101 = FetchMesh(b + uint3(1, 0, 1));
  float c011 = FetchMesh(b + uint3(0, 1, 1));
  float c111 = FetchMesh(b + uint3(1, 1, 1));
  float x00 = lerp(c000, c100, f.x), x10 = lerp(c010, c110, f.x);
  float x01 = lerp(c001, c101, f.x), x11 = lerp(c011, c111, f.x);
  return lerp(lerp(x00, x10, f.y), lerp(x01, x11, f.y), f.z);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
  uint res = kSdfRes;  // clip resolution is a code constant
  if (any(id >= res)) return;

  uint clip = pc.mesh_res.w;
  float clip_voxel = pc.clip_origin.w;
  float3 world = pc.clip_origin.xyz + (float3(id) + 0.5) * clip_voxel;
  float3 local = mul(pc.inv_transform, float4(world, 1.0)).xyz;

  // Mesh-voxel coordinate; mesh volume spans [box_min, box_min + res*voxel].
  float mesh_voxel = pc.box_min.w;
  float3 mc = (local - pc.box_min.xyz) / mesh_voxel;  // [0, res]
  float3 res_f = float3(pc.mesh_res.xyz);

  float d_local;
  if (all(mc >= 0.0) && all(mc <= res_f)) {
    d_local = SampleMesh(mc);
  } else {
    // Outside the volume box: a PROVEN lower bound on the distance to the mesh
    // surface, so the sphere trace never overshoots (the previous box_dist+edge
    // was an upper bound and could step through geometry). Two lower bounds,
    // take the larger:
    //  (a) box_dist -- the surface lies inside the mesh AABB, so the distance
    //      from p to the box is <= the distance from p to the surface.
    //  (b) 1-Lipschitz SDF: d(p) >= d(q) - |p-q| for any q. Clamping to the AABB
    //      makes q the closest box point, and |p-q| == box_dist exactly, so
    //      d(p) >= SampleMesh(q) - box_dist (may be negative; (a) covers that).
    float3 clamped = clamp(mc, 0.0, res_f);
    float3 diff = (mc - clamped) * mesh_voxel;  // local-space offset p - q
    float box_dist = length(diff);
    float edge = SampleMesh(clamped);           // d(q)
    d_local = max(box_dist, edge - box_dist);
  }

  float d_world = d_local * pc.misc.x;  // conservative local->world scale

  uint3 texel = uint3(id.x, id.y, clip * res + id.z);
  float current = dist_vol[texel];
  if (d_world < current) {
    dist_vol[texel] = d_world;
    albedo_vol[texel] = float4(pc.albedo.rgb, 1.0);
    emissive_vol[texel] = float4(pc.emissive.rgb, 1.0);
  }
}
