#include "rhi_bindings.hlsli"
// Froxel light culling: one thread per cluster, brute-force sphere-vs-AABB
// over the frame's dynamic lights (256 max - cheap enough that smarter
// two-level culling can wait). Each cluster owns a fixed slice of the index
// buffer, so there are no atomics and the forward loop reads a compact list.
struct ClusterPush {
  column_major float4x4 view;  // world -> view
  float2 screen;               // render resolution
  float near_plane;
  float slice_scale;           // slice = log2(viewz) * scale + bias
  float slice_bias;
  uint light_count;
  float tan_half_fov_y;
  float aspect;
  uint decal_count;
  float3 pad;
};
PUSH_CONSTANTS(ClusterPush, pc);

struct Light {
  float4 pos_radius;
  float4 color_intensity;
  float4 direction_type;
  float4 params;
};
struct Decal {
  float4 row0;  // world -> unit box
  float4 row1;
  float4 row2;
  float4 uv_rect;
  float4 tint_blend;
  float4 params2;  // normal strength, roughness mult, emissive, pad
};
[[vk::binding(0, 0)]] StructuredBuffer<Light> lights : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> cluster_counts : register(u1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> cluster_indices : register(u2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<Decal> decals : register(t3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> decal_indices : register(u4, space0);

static const uint kTilesX = 16;
static const uint kTilesY = 9;
static const uint kSlices = 24;
static const uint kMaxPerCluster = 32;
static const uint kMaxDecalsPerCluster = 16;

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint cluster = id.x;
  if (cluster >= kTilesX * kTilesY * kSlices) return;
  uint tx = cluster % kTilesX;
  uint ty = (cluster / kTilesX) % kTilesY;
  uint tz = cluster / (kTilesX * kTilesY);

  // Froxel AABB in view space (+x right, +y up, -z forward). Exponential z
  // slices; the xy extent scales with depth through the projection.
  float z0 = exp2((float(tz) - pc.slice_bias) / pc.slice_scale);
  float z1 = exp2((float(tz) + 1.0 - pc.slice_bias) / pc.slice_scale);
  // ndc extents of the tile
  float nx0 = (float(tx) / kTilesX) * 2.0 - 1.0;
  float nx1 = (float(tx + 1u) / kTilesX) * 2.0 - 1.0;
  float ny0 = (float(ty) / kTilesY) * 2.0 - 1.0;
  float ny1 = (float(ty + 1u) / kTilesY) * 2.0 - 1.0;
  float hx = pc.tan_half_fov_y * pc.aspect;
  float hy = pc.tan_half_fov_y;
  float3 mn, mx;
  mn.z = -z1;
  mx.z = -z0;
  mn.x = min(min(nx0 * hx * z0, nx0 * hx * z1), min(nx1 * hx * z0, nx1 * hx * z1));
  mx.x = max(max(nx0 * hx * z0, nx0 * hx * z1), max(nx1 * hx * z0, nx1 * hx * z1));
  // vulkan ndc y-down: ndc -1 is the TOP row; view-space y is up
  mn.y = min(min(-ny0 * hy * z0, -ny0 * hy * z1), min(-ny1 * hy * z0, -ny1 * hy * z1));
  mx.y = max(max(-ny0 * hy * z0, -ny0 * hy * z1), max(-ny1 * hy * z0, -ny1 * hy * z1));

  uint count = 0;
  for (uint li = 0; li < pc.light_count && count < kMaxPerCluster; ++li) {
    Light l = lights[li];
    float3 c = mul(pc.view, float4(l.pos_radius.xyz, 1.0)).xyz;
    float r = l.pos_radius.w;
    float3 closest = clamp(c, mn, mx);
    float3 d = c - closest;
    if (dot(d, d) > r * r) continue;
    cluster_indices[cluster * kMaxPerCluster + count] = li;
    ++count;
  }

  // Decals: conservative bounding-sphere test of the oriented box. The box
  // rows are world->unit, so the world-space half-extent along each axis is
  // 1/|row| and the center is -rows^T * offsets... cheaper: bound by the
  // largest half-extent around the reconstructed center.
  uint dcount = 0;
  for (uint di = 0; di < pc.decal_count && dcount < kMaxDecalsPerCluster; ++di) {
    Decal dec = decals[di];
    // center: solve rows * c + offset = 0 => c = -M^-1 * offset. For the
    // scaled-rotation rows M = S*R, M^-1 = R^T * S^-1; column k of M^-1 is
    // row k / |row k|^2.
    float3 r0 = dec.row0.xyz, r1 = dec.row1.xyz, r2 = dec.row2.xyz;
    float3 center = -(r0 / dot(r0, r0) * dec.row0.w + r1 / dot(r1, r1) * dec.row1.w +
                      r2 / dot(r2, r2) * dec.row2.w);
    float radius = sqrt(1.0 / dot(r0, r0) + 1.0 / dot(r1, r1) + 1.0 / dot(r2, r2));
    float3 c = mul(pc.view, float4(center, 1.0)).xyz;
    float3 closest = clamp(c, mn, mx);
    float3 d = c - closest;
    if (dot(d, d) > radius * radius) continue;
    decal_indices[cluster * kMaxDecalsPerCluster + dcount] = di;
    ++dcount;
  }
  cluster_counts[cluster] = count | (dcount << 16);
}
