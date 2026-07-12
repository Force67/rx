#include "rhi_bindings.hlsli"
#include "vgeo_common.hlsli"
// Virtual geometry cluster culling, both phases of the two-pass occlusion
// scheme (push.mode selects):
//
//   mode 0 (main): one thread per cluster x instance evaluates the DAG cut
//     (project(self_error) <= tau < project(parent_error)), then frustum and
//     backface-cone culls the survivors. Clusters that pass everything but the
//     PREVIOUS frame's hi-z go to the occluded list instead of being dropped:
//     they were hidden last frame, but the camera may have moved.
//   mode 1 (post): one thread per occluded-list entry retests against the hi-z
//     rebuilt from THIS frame's depth (scene + main-raster visibility buffer).
//     Survivors are disoccluded clusters that draw in the second raster pass.
//
// Passing clusters append to the shared visible list and, by projected screen
// size, to either the compute-raster (sw) or mesh-shader (hw) index list.
// Clusters that cross the near plane always go hw: the fixed-function clipper
// handles them, the compute rasterizer does not clip.

[[vk::binding(0, 0)]] StructuredBuffer<VgeoParams> params_buf : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<DagMeshlet> meshlets : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<VgeoInstance> instances : register(t2, space0);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint2> visible : register(u3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> sw_list : register(u4, space0);
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> hw_list : register(u5, space0);
[[vk::binding(6, 0)]] RWStructuredBuffer<uint2> occluded : register(u6, space0);
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> counters : register(u7, space0);
[[vk::binding(8, 0)]] Texture2D<float> hiz : register(t8, space0);  // farthest depth

struct PushData {
  uint mode;  // 0 = main (prev-frame hi-z), 1 = post (current-frame hi-z)
};
PUSH_CONSTANTS(PushData, push);

// Conservative screen-space size (pixels) of a world-space error at the
// sphere; inside the sphere everything stays full detail.
float ProjectError(VgeoParams p, float3 center, float radius, float error) {
  float d = max(length(center - p.camera.xyz) - radius, 0.1);
  return error * p.camera.w / d;
}

// Sphere vs a coarse farthest-depth hi-z. view_proj/eye pick the frame the
// map was captured in. Returns true when the sphere is provably behind every
// occluder over its footprint; anything untestable (behind the eye, footprint
// too large for the coarse map) counts as visible.
bool HizOccluded(float3 center, float radius, Texture2D<float> map, float2 map_size,
                 float2 proj_diag, column_major float4x4 view_proj, float3 eye) {
  float4 cc = mul(view_proj, float4(center, 1.0));
  if (cc.w <= 1e-4) return false;
  float3 ndc = cc.xyz / cc.w;
  float dist = max(length(center - eye), 1e-3);
  float sr = radius * max(proj_diag.x, proj_diag.y) / dist;  // ndc radius
  float3 near_pt = center + normalize(eye - center) * radius;
  float4 nc = mul(view_proj, float4(near_pt, 1.0));
  float nearest_z = nc.z / max(nc.w, 1e-4);  // reversed z: larger = closer

  int2 t_lo = int2(((ndc.xy - sr) * 0.5 + 0.5) * map_size);
  int2 t_hi = int2(((ndc.xy + sr) * 0.5 + 0.5) * map_size);
  const int kMaxTexels = 8;
  if (t_lo.x < 0 || t_lo.y < 0 || t_hi.x >= int(map_size.x) || t_hi.y >= int(map_size.y) ||
      (t_hi.x - t_lo.x) > kMaxTexels || (t_hi.y - t_lo.y) > kMaxTexels) {
    return false;
  }
  float farthest = 1.0;  // reversed z: smallest value over the footprint
  for (int y = t_lo.y; y <= t_hi.y; ++y) {
    for (int x = t_lo.x; x <= t_hi.x; ++x) {
      farthest = min(farthest, map.Load(int3(x, y, 0)));
    }
  }
  return nearest_z < farthest;
}

void Emit(VgeoParams p, uint cluster, uint instance, float3 center, float radius,
          uint tri_count, bool near_clip) {
  uint vi;
  InterlockedAdd(counters[VGEO_COUNTER_VISIBLE], 1, vi);
  if (vi >= p.max_visible) return;
  visible[vi] = uint2(cluster, instance);

  float dist = max(length(center - p.camera.xyz), 1e-3);
  float screen_radius = radius * p.camera.w / dist;
  // The DAG cut keeps cluster footprints roughly constant on screen, so the
  // binning metric is the projected TRIANGLE size, not the cluster size:
  // radius/sqrt(tris) tracks the average edge. Small triangles raster faster
  // in compute than through fixed-function setup.
  float edge_estimate = 2.0 * screen_radius * rsqrt(max(float(tri_count), 1.0));
  if (!near_clip && edge_estimate < float(p.sw_threshold)) {
    uint s;
    InterlockedAdd(counters[VGEO_COUNTER_SW], 1, s);
    if (s < p.max_visible) sw_list[s] = vi;
  } else {
    uint h;
    InterlockedAdd(counters[VGEO_COUNTER_HW], 1, h);
    if (h < p.max_visible) hw_list[h] = vi;
  }
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  VgeoParams p = params_buf[0];

  uint cluster, instance;
  if (push.mode == 0) {
    uint total = p.cluster_count * p.instance_count;
    if (id.x >= total) return;
    cluster = id.x % p.cluster_count;
    instance = id.x / p.cluster_count;
  } else {
    if (id.x >= counters[VGEO_COUNTER_OCCLUDED]) return;
    uint2 entry = occluded[id.x];
    cluster = entry.x;
    instance = entry.y;
  }

  DagMeshlet m = meshlets[cluster];
  VgeoInstance inst = instances[instance];
  float scale = max(length(inst.model[0].xyz),
                    max(length(inst.model[1].xyz), length(inst.model[2].xyz)));

  if (push.mode == 0) {
    // DAG cut on the group spheres in world space: a cluster and the parents
    // that replace it share the exact same (sphere, error) pair on their
    // boundary, so per-region level selection cannot leave gaps.
    float3 self_c = mul(inst.model, float4(m.self_sphere.xyz, 1.0)).xyz;
    float3 parent_c = mul(inst.model, float4(m.parent_sphere.xyz, 1.0)).xyz;
    float tau = p.prev_camera.w;
    bool selected =
        ProjectError(p, self_c, m.self_sphere.w * scale, m.self_error * scale) <= tau &&
        (m.parent_error >= 1e30 ||
         ProjectError(p, parent_c, m.parent_sphere.w * scale, m.parent_error * scale) > tau);
    if (!selected) return;
  }

  float3 center = mul(inst.model, float4(m.center_radius.xyz, 1.0)).xyz;
  float radius = m.center_radius.w * scale;

  if (push.mode == 0) {
    [unroll]
    for (int pl = 0; pl < 5; ++pl) {
      if (dot(p.planes[pl].xyz, center) + p.planes[pl].w < -radius) return;
    }
    // Backface cone (model-space axis; valid for rotation + uniform scale).
    float3 axis = normalize(mul((float3x3)inst.model, m.cone.xyz));
    float3 view_dir = normalize(center - p.camera.xyz);
    if (dot(view_dir, axis) >= m.cone.w) return;
  }

  // near_clip: the sphere reaches past the near plane, so the cluster must go
  // through the fixed-function clipper. Tested on clip w (the view depth) -
  // planes[4] degenerates to {0,0,0,near} under the reversed-z infinite-far
  // projection and cannot answer this.
  float view_depth = mul(p.view_proj, float4(center, 1.0)).w;
  bool near_clip = view_depth < radius + p.hiz.z;

  if (push.mode == 0) {
    if (p.prev_hiz.x > 0.0 &&
        HizOccluded(center, radius, hiz, p.prev_hiz.xy, p.prev_hiz.zw, p.prev_view_proj,
                    p.prev_camera.xyz)) {
      uint o;
      InterlockedAdd(counters[VGEO_COUNTER_OCCLUDED], 1, o);
      if (o < p.max_visible) occluded[o] = uint2(cluster, instance);
      return;
    }
  } else {
    if (HizOccluded(center, radius, hiz, p.hiz.xy, p.prev_hiz.zw, p.view_proj, p.camera.xyz)) {
      return;
    }
  }

  Emit(p, cluster, instance, center, radius, m.triangle_count, near_clip);
}
