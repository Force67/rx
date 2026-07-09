#include "rhi_bindings.hlsli"
// Virtual geometry mesh shader: one workgroup per DAG cluster. The workgroup
// first evaluates the LOD cut - draw iff
//   project(self_error) <= tau < project(parent_error)
// (screen-space error against the cluster / parent-group bounding spheres) -
// then frustum- and backface-cone-culls the survivors and emits the cluster
// geometry. Errors are monotonic up the DAG and group borders were locked
// during simplification, so exactly one level covers every surface point and
// adjacent clusters from neighboring levels share identical boundary edges.
struct DagMeshlet {
  float4 center_radius;  // xyz bounding-sphere center, w radius
  float4 cone;           // xyz normal-cone axis, w cutoff (>=2 disables)
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
  float4 self_sphere;    // group sphere this cluster was simplified from
  float4 parent_sphere;  // sphere of the group that replaces it
  float self_error;
  float parent_error;    // FLT_MAX at the DAG roots
  uint lod;
  uint pad;
};
struct MVertex {
  float px, py, pz;
  float nx, ny, nz;
};

[[vk::binding(0, 0)]] StructuredBuffer<DagMeshlet> meshlets : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<uint> meshlet_vertices : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint> meshlet_triangles : register(t2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<MVertex> vertices : register(t3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> visible_counter : register(u4, space0);

struct PushData {
  column_major float4x4 view_proj;
  float4 planes[5];
  float4 camera;       // xyz eye, w proj_scale (px per world unit at d = 1)
  float error_pixels;  // tau
  uint meshlet_count;
  float pad0;
  float pad1;
};
PUSH_CONSTANTS(PushData, push);

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] nointerpolation uint meshlet : MESHLET;
};

// Conservative screen-space size (pixels) of a world-space error at the
// sphere; inside the sphere everything stays full detail.
float ProjectError(float3 center, float radius, float error) {
  float d = max(length(center - push.camera.xyz) - radius, 0.1);
  return error * push.camera.w / d;
}

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex,
          out vertices VsOut verts[64], out indices uint3 tris[124]) {
  uint mi = gid.x;
  DagMeshlet m = meshlets[mi];

  // DAG cut: this cluster's own error must be invisible, and its parent's
  // must not be (otherwise the parent level covers this region).
  bool selected =
      ProjectError(m.self_sphere.xyz, m.self_sphere.w, m.self_error) <= push.error_pixels &&
      (m.parent_error >= 1e30 ||
       ProjectError(m.parent_sphere.xyz, m.parent_sphere.w, m.parent_error) > push.error_pixels);

  float3 c = m.center_radius.xyz;
  float r = m.center_radius.w;
  bool culled = !selected;
  [unroll]
  for (int p = 0; p < 5; ++p) {
    if (dot(push.planes[p].xyz, c) + push.planes[p].w < -r) culled = true;
  }
  float3 view_dir = normalize(c - push.camera.xyz);
  if (dot(view_dir, m.cone.xyz) >= m.cone.w) culled = true;
  SetMeshOutputCounts(culled ? 0 : m.vertex_count, culled ? 0 : m.triangle_count);
  if (culled) return;
  if (tid == 0) InterlockedAdd(visible_counter[0], 1);

  for (uint v = tid; v < m.vertex_count; v += 128) {
    uint vid = meshlet_vertices[m.vertex_offset + v];
    MVertex mv = vertices[vid];
    VsOut o;
    o.pos = mul(push.view_proj, float4(mv.px, mv.py, mv.pz, 1.0));
    o.normal = float3(mv.nx, mv.ny, mv.nz);
    o.meshlet = mi;
    verts[v] = o;
  }
  for (uint t = tid; t < m.triangle_count; t += 128) {
    uint packed = meshlet_triangles[m.triangle_offset + t];
    tris[t] = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
  }
}
