#ifndef RX_RENDER_VIRTUAL_GEOMETRY_H_
#define RX_RENDER_VIRTUAL_GEOMETRY_H_

// Virtual geometry: a cluster-DAG LOD hierarchy rendered through a fully
// GPU-driven, two-pass occlusion-culled visibility buffer.
//
// Build (Upload): meshlets are grouped by spatial adjacency, each group's
// interior is QEM-simplified with its border vertices LOCKED (so any two
// clusters that can meet across a level boundary share identical edges), and
// the result re-clusters into the next level - repeated until a handful of
// root clusters remain. A cluster draws iff
//   project(self_error) <= tau < project(parent_error)
// - the standard DAG cut: each screen region independently picks the coarsest
// level whose error is invisible, per-cluster LOD inside one mesh, no cracks.
//
// Frame (AddToGraph, gpu-driven path):
//   1 clear      visibility buffer + counters
//   2 cull main  DAG cut + frustum + cone per cluster x instance; passing
//                clusters split into compute-raster (small) and mesh-shader
//                (large / near-clipping) bins. Clusters that fail only the
//                PREVIOUS frame's hi-z are deferred, not dropped.
//   3 raster     sw: one 128-thread group per cluster rasters its triangles
//                with 2D edge functions; hw: mesh shader + pixel shader.
//                Both write (depth<<32 | cluster | tri) with one 64-bit
//                InterlockedMax - the atomic IS the depth test.
//   4 hi-z       coarse farthest-depth rebuilt from scene depth + step 3
//   5 cull post  retests the deferred clusters against the fresh hi-z;
//                disocclusions raster exactly like step 3
//   6 resolve    fullscreen: decode ids, refetch verts, perspective-correct
//                barycentrics, shade, export SV_Depth against the scene
//
// Falls back to the single-pass mesh-shader path (no occlusion, no sw raster)
// when the device lacks 64-bit buffer atomics; stays inert without mesh
// shaders. Cluster pages/streaming are deliberately out of scope here: the
// whole DAG stays resident.

#include <span>

#include "asset/mesh.h"
#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

// Mirrors DagMeshlet in vgeo_common.hlsli (std430, 96 bytes). The LOD cut
// compares against self/parent group spheres+errors, NOT the render bounds: a
// cluster and the parents that replace it share the exact same (sphere, error)
// pair on the boundary between them, which is what makes the cut gap-free.
struct DagMeshlet {
  f32 center_radius[4];  // xyz bounding-sphere center, w radius (culling)
  f32 cone[4];           // xyz axis, w cutoff (>=2 disables cone cull)
  u32 vertex_offset = 0;
  u32 triangle_offset = 0;
  u32 vertex_count = 0;
  u32 triangle_count = 0;
  f32 self_sphere[4];    // group sphere this cluster was simplified from
  f32 parent_sphere[4];  // sphere of the group that replaces it
  f32 self_error = 0;    // error of the simplification that created it (lod0: 0)
  f32 parent_error = 0;  // error of the replacing group (FLT_MAX = root)
  u32 lod = 0;
  u32 pad = 0;
};

class VirtualGeometryPass {
 public:
  static constexpr u32 kMaxInstances = 256;

  bool Initialize(Device& device, Format color_format, Format depth_format);
  void Destroy(Device& device);

  // Builds the full cluster DAG from the mesh's lod 0 and uploads it.
  void Upload(Device& device, const asset::Mesh& mesh);
  // World transforms drawn next frame; empty keeps a single identity instance.
  void SetInstances(std::span<const Mat4> transforms);

  bool active() const { return meshlet_count_ > 0; }
  bool gpu_driven() const { return gpu_driven_; }
  u32 meshlet_count() const { return meshlet_count_; }
  u32 lod_count() const { return lod_count_; }
  u32 instance_count() const { return instance_count_; }
  // Previous frame's cull results (one frame stale, fence-safe):
  // {visible, sw rastered, hw rastered, deferred-then-retested}.
  struct Stats {
    u32 visible = 0;
    u32 sw = 0;
    u32 hw = 0;
    u32 occluded = 0;
  };
  Stats last_stats(u32 slot) const;
  u32 last_visible(u32 slot) const { return last_stats(slot).visible; }

  struct Frame {
    Mat4 view_proj;
    f32 planes[5][4];
    Vec3 eye;
    f32 proj_scale = 1;   // screen px per world unit at distance 1
    f32 proj_m00 = 1;     // |proj[0][0]|, |proj[1][1]| for ndc-radius estimates
    f32 proj_m11 = 1;
    f32 error_pixels = 1;  // tau: screen-space error budget
    f32 near_plane = 0.1f;  // for the near-crossing (clipper-needed) test
    ResourceHandle color = kInvalidResource;  // lit target, loaded
    ResourceHandle depth = kInvalidResource;  // scene depth, loaded
    u32 width = 0;   // render extent (the graph's transient images have no
    u32 height = 0;  // physical extent until Compile)
    u32 debug = 0;  // resolve tint: 0 shaded, 1 cluster, 2 lod, 3 sw/hw
    u32 slot = 0;
  };
  void AddToGraph(Device& device, RenderGraph& graph, const Frame& frame);

 private:
  static constexpr u32 kFramesInFlight = 2;
  static constexpr u32 kMaxVisible = 1u << 20;
  static constexpr u32 kHizDownsample = 8;
  static constexpr u32 kCounterSlots = 8;

  void AddLegacyPass(RenderGraph& graph, const Frame& frame);
  void EnsureTargets(Device& device, u32 width, u32 height);

  bool available_ = false;   // mesh shaders present
  bool gpu_driven_ = false;  // + 64-bit buffer atomics
  PipelineHandle legacy_pipeline_;
  PipelineHandle cull_pipeline_;
  PipelineHandle args_pipeline_;
  PipelineHandle clear_pipeline_;
  PipelineHandle sw_pipeline_;
  PipelineHandle hzb_pipeline_;
  PipelineHandle vis_pipeline_;
  PipelineHandle resolve_pipeline_;

  // DAG (rebuilt per Upload)
  GpuBuffer meshlets_;
  GpuBuffer meshlet_vertices_;
  GpuBuffer meshlet_triangles_;
  GpuBuffer vertices_;
  u32 meshlet_count_ = 0;
  u32 lod_count_ = 0;

  // Frame plumbing
  GpuBuffer params_[kFramesInFlight];     // host-visible, mirrors VgeoParams
  GpuBuffer instances_[kFramesInFlight];  // host-visible instance transforms
  GpuBuffer visible_;
  GpuBuffer sw_list_;
  GpuBuffer hw_list_;
  GpuBuffer occluded_;
  GpuBuffer counters_;
  GpuBuffer args_;
  GpuBuffer readback_[kFramesInFlight];
  GpuBuffer legacy_counters_[kFramesInFlight];
  GpuBuffer visbuffer_;
  GpuImage dummy_hiz_;
  // Own hi-z ping-pong: the map built this frame (scene depth + visibility
  // buffer, rebuilt again after the post raster so it is complete) is next
  // frame's main-pass occlusion source. Self-contained: no dependency on when
  // the renderer snapshots depth, and it always includes our own geometry.
  GpuImage hiz_img_[kFramesInFlight];
  ResourceState hiz_state_[kFramesInFlight] = {ResourceState::kUndefined,
                                               ResourceState::kUndefined};
  u32 hiz_dims_[kFramesInFlight][2] = {{0, 0}, {0, 0}};
  u32 vis_width_ = 0;
  u32 vis_height_ = 0;

  base::Vector<Mat4> pending_instances_;
  u32 instance_count_ = 1;

  // Last frame's view, for the prev-hi-z test in the main cull.
  Mat4 prev_view_proj_;
  Vec3 prev_eye_{};
  bool has_prev_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_VIRTUAL_GEOMETRY_H_
