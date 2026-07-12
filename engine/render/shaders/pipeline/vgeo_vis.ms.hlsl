#include "rhi_bindings.hlsli"
#include "vgeo_common.hlsli"
// Hardware-raster half of the visibility pass: one workgroup per hw-list
// entry emits the cluster's triangles with only a position and a packed
// per-primitive id (visible-list index + local triangle). Culling already
// happened in the compute cull, so this shader just fetches and emits; the
// paired pixel shader resolves into the 64-bit visibility buffer, no
// attachments are bound.

[[vk::binding(0, 0)]] StructuredBuffer<VgeoParams> params_buf : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<DagMeshlet> meshlets : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint> meshlet_vertices : register(t2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint> meshlet_triangles : register(t3, space0);
[[vk::binding(4, 0)]] StructuredBuffer<MVertex> vertices : register(t4, space0);
[[vk::binding(5, 0)]] StructuredBuffer<VgeoInstance> instances : register(t5, space0);
[[vk::binding(6, 0)]] StructuredBuffer<uint2> visible : register(t6, space0);
[[vk::binding(7, 0)]] StructuredBuffer<uint> hw_list : register(t7, space0);
[[vk::binding(9, 0)]] StructuredBuffer<uint> counters : register(t9, space0);

struct PushData {
  uint mode;  // 0 = main pass (list from 0), 1 = post pass (from hw base)
};
PUSH_CONSTANTS(PushData, push);

struct VsOut {
  float4 pos : SV_Position;
};
struct PrimOut {
  uint id : SV_PrimitiveID;  // (visible index + 1) << 7 | triangle
};

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex,
          out vertices VsOut verts[64], out indices uint3 tris[124],
          out primitives PrimOut prims[124]) {
  VgeoParams p = params_buf[0];
  uint base = push.mode != 0 ? counters[VGEO_COUNTER_HW_BASE] : 0;
  uint vi = hw_list[base + gid.x];
  uint2 entry = visible[vi];
  DagMeshlet m = meshlets[entry.x];
  VgeoInstance inst = instances[entry.y];

  SetMeshOutputCounts(m.vertex_count, m.triangle_count);

  for (uint v = tid; v < m.vertex_count; v += 128) {
    MVertex mv = vertices[meshlet_vertices[m.vertex_offset + v]];
    float4 world = mul(inst.model, float4(mv.px, mv.py, mv.pz, 1.0));
    VsOut o;
    o.pos = mul(p.view_proj, world);
    verts[v] = o;
  }
  for (uint t = tid; t < m.triangle_count; t += 128) {
    uint packed = meshlet_triangles[m.triangle_offset + t];
    tris[t] = uint3(packed & 0xffu, (packed >> 8) & 0xffu, (packed >> 16) & 0xffu);
    PrimOut prim;
    prim.id = ((vi + 1u) << 7) | t;
    prims[t] = prim;
  }
}
