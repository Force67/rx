// Shared declarations for the GPU-driven virtual geometry pipeline: the
// cluster DAG record, the per-frame parameter block, the visible-cluster
// encoding and the 64-bit visibility-buffer packing.
//
// The visibility buffer holds one uint64 per pixel:
//   [63:32] depth bits (reversed-z float, asuint - order-preserving)
//   [31:7]  visible-list index + 1 (0 = empty pixel)
//   [6:0]   triangle index within the cluster (< 124)
// Both rasterizers (compute and mesh-shader) resolve overlap with a single
// InterlockedMax, so the nearest surface wins regardless of submission order.
#ifndef RX_VGEO_COMMON_HLSLI_
#define RX_VGEO_COMMON_HLSLI_

struct DagMeshlet {
  float4 center_radius;  // xyz bounding-sphere center, w radius (model space)
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

struct VgeoInstance {
  column_major float4x4 model;
};

// Mirrors render::VirtualGeometryPass::Params (std430).
struct VgeoParams {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;  // last frame, for the prev-hiz test
  float4 planes[5];    // frustum planes, inside >= 0
  float4 camera;       // xyz eye, w proj_scale (px per world unit at d = 1)
  float4 prev_camera;  // xyz last frame's eye, w tau (error budget, pixels)
  float4 prev_hiz;     // prev-frame hi-z w, h (0 = no occlusion), proj m00, m11
  float4 hiz;          // own current-frame hi-z w, h; z = near plane distance
  uint cluster_count;   // clusters per mesh
  uint instance_count;
  uint width;           // render target extent
  uint height;
  uint sw_threshold;    // screen radius (px) below which a cluster rasters in compute
  uint debug;           // resolve tint: 0 shaded, 1 cluster, 2 lod, 3 raster path
  uint max_visible;     // capacity of the visible / occluded lists
  uint pad;
};

// counters[] slots (all cleared by vgeo_clear, appended with InterlockedAdd)
#define VGEO_COUNTER_VISIBLE 0   // entries in the visible list
#define VGEO_COUNTER_SW 1        // entries in the sw index list
#define VGEO_COUNTER_HW 2        // entries in the hw index list
#define VGEO_COUNTER_OCCLUDED 3  // main-pass hi-z failures to retest
#define VGEO_COUNTER_SW_BASE 4   // sw list size when the main raster ran
#define VGEO_COUNTER_HW_BASE 5   // hw list size when the main raster ran

// args[] u32 layout: indirect records written by vgeo_args
#define VGEO_ARGS_SW_MAIN 0    // {x,y,z} compute dispatch, one group per cluster
#define VGEO_ARGS_HW_MAIN 3    // {x,y,z} mesh-task dispatch
#define VGEO_ARGS_POST_CULL 6  // {x,y,z} compute dispatch, 64 clusters per group
#define VGEO_ARGS_SW_POST 9
#define VGEO_ARGS_HW_POST 12

// sw flag: bit 31 of the payload marks compute-rasterized pixels (free bits:
// the visible list is capped well below 2^24, so (index+1)<<7 stays under
// bit 31). Purely diagnostic, the resolve masks it off before decoding.
#define VGEO_PAYLOAD_SW (1u << 31)

uint64_t VgeoPack(float depth, uint visible_index, uint tri, uint flags) {
  uint payload = ((visible_index + 1u) << 7) | (tri & 0x7fu) | flags;
  return (uint64_t(asuint(saturate(depth))) << 32) | uint64_t(payload);
}

// A visible-list entry: x = cluster index, y = instance index.
// The list is shared by both raster binners; sw/hw index lists point into it.

float3 VgeoMeshletColor(uint i) {
  uint h = i * 2654435761u;
  return float3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) / 255.0;
}

#endif  // RX_VGEO_COMMON_HLSLI_
