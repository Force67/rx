#include "rhi_bindings.hlsli"
#include "vgeo_common.hlsli"
// Compute rasterizer for small clusters: one 128-thread group per sw-list
// entry. The group transforms the cluster's vertices to screen once (shared
// memory), then rasters one triangle per thread with 2D edge functions and
// resolves each covered pixel with a single 64-bit InterlockedMax into the
// visibility buffer. Sub-pixel triangles cost a few edge tests instead of
// fixed-function raster setup, which is the whole point: the cull sends only
// clusters whose screen footprint is below sw_threshold pixels, so per
// triangle the bounding box is a handful of pixels.
//
// The cull keeps near-plane-crossing clusters on the mesh-shader path, so no
// clipping happens here; w is safely positive for every vertex. NDC z (z/w)
// is linear in screen space, so depth interpolates with the plain 2D
// barycentrics - no perspective correction needed for the visibility pass.

[[vk::binding(0, 0)]] StructuredBuffer<VgeoParams> params_buf : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<DagMeshlet> meshlets : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint> meshlet_vertices : register(t2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint> meshlet_triangles : register(t3, space0);
[[vk::binding(4, 0)]] StructuredBuffer<MVertex> vertices : register(t4, space0);
[[vk::binding(5, 0)]] StructuredBuffer<VgeoInstance> instances : register(t5, space0);
[[vk::binding(6, 0)]] StructuredBuffer<uint2> visible : register(t6, space0);
[[vk::binding(7, 0)]] StructuredBuffer<uint> sw_list : register(t7, space0);
[[vk::binding(8, 0)]] RWStructuredBuffer<uint64_t> visbuffer : register(u8, space0);
[[vk::binding(9, 0)]] StructuredBuffer<uint> counters : register(t9, space0);

struct PushData {
  uint mode;  // 0 = main pass (list from 0), 1 = post pass (from sw base)
};
PUSH_CONSTANTS(PushData, push);

groupshared float3 gs_screen[64];  // xy pixels, z ndc depth

[numthreads(128, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
  VgeoParams p = params_buf[0];
  uint base = push.mode != 0 ? counters[VGEO_COUNTER_SW_BASE] : 0;
  uint vi = sw_list[base + gid.x];
  uint2 entry = visible[vi];
  DagMeshlet m = meshlets[entry.x];
  VgeoInstance inst = instances[entry.y];

  for (uint v = tid; v < m.vertex_count; v += 128) {
    MVertex mv = vertices[meshlet_vertices[m.vertex_offset + v]];
    float4 world = mul(inst.model, float4(mv.px, mv.py, mv.pz, 1.0));
    float4 clip = mul(p.view_proj, world);
    float w = max(clip.w, 1e-6);  // never near-crossing on this path
    float2 pixel = (clip.xy / w * 0.5 + 0.5) * float2(p.width, p.height);
    gs_screen[v] = float3(pixel, clip.z / w);
  }
  GroupMemoryBarrierWithGroupSync();

  for (uint t = tid; t < m.triangle_count; t += 128) {
    uint packed = meshlet_triangles[m.triangle_offset + t];
    float3 v0 = gs_screen[packed & 0xffu];
    float3 v1 = gs_screen[(packed >> 8) & 0xffu];
    float3 v2 = gs_screen[(packed >> 16) & 0xffu];

    // Signed area in framebuffer coords (y down): the engine's counter-
    // clockwise front faces come out negative, so positive area is a back
    // face (matches the hardware path's kBack cull, verified against it).
    float area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
    if (area >= 0.0) continue;

    float2 lo = min(v0.xy, min(v1.xy, v2.xy));
    float2 hi = max(v0.xy, max(v1.xy, v2.xy));
    int2 p_lo = int2(max(floor(lo + 0.5), 0.0));
    int2 p_hi = int2(min(ceil(hi - 0.5), float2(p.width - 1, p.height - 1)));
    if (p_lo.x > p_hi.x || p_lo.y > p_hi.y) continue;
    // A raster bug (or a giant cluster misrouted here) must not TDR: the cull
    // only sends sub-sw_threshold clusters, so triangles are a few pixels.
    if ((p_hi.x - p_lo.x) > 128 || (p_hi.y - p_lo.y) > 128) continue;

    float inv_area = 1.0 / area;
    uint payload_vi = vi;
    for (int y = p_lo.y; y <= p_hi.y; ++y) {
      for (int x = p_lo.x; x <= p_hi.x; ++x) {
        float2 s = float2(x + 0.5, y + 0.5);
        float e0 = (v1.x - s.x) * (v2.y - s.y) - (v1.y - s.y) * (v2.x - s.x);
        float e1 = (v2.x - s.x) * (v0.y - s.y) - (v2.y - s.y) * (v0.x - s.x);
        float e2 = (v0.x - s.x) * (v1.y - s.y) - (v0.y - s.y) * (v1.x - s.x);
        // Same sign as the (negative) area = inside. Zero-edges stay in so
        // shared edges never leave seams; the atomic max dedupes the overlap.
        if (e0 > 0.0 || e1 > 0.0 || e2 > 0.0) continue;
        float depth = (e0 * v0.z + e1 * v1.z + e2 * v2.z) * inv_area;
        InterlockedMax(visbuffer[uint(y) * p.width + uint(x)],
                       VgeoPack(depth, payload_vi, t, VGEO_PAYLOAD_SW));
      }
    }
  }
}
