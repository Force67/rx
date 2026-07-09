#ifndef RX_RENDER_VIRTUAL_GEOMETRY_H_
#define RX_RENDER_VIRTUAL_GEOMETRY_H_

// Nanite-class virtual geometry core: a cluster-DAG LOD hierarchy over the
// meshlet path. At upload, meshlets are grouped (morton runs), each group's
// interior is QEM-simplified with its border vertices LOCKED (so any two
// clusters that can meet across a level boundary share identical edges), and
// the result re-clusters into the next level - repeated until a handful of
// root clusters remain. At runtime one mesh-shader workgroup per cluster
// evaluates the standard DAG cut - draw iff
//   project(self_error) <= tau < project(parent_error)
// - so each screen region independently picks the coarsest level whose error
// is invisible, giving per-cluster LOD inside a single mesh with no cracks.
// Self-contained demo path (--demo vgeo), like the flat meshlet pass.

#include "asset/mesh.h"
#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

// Mirrors DagMeshlet in vgeo.ms (std430, 96 bytes). The LOD cut compares
// against self/parent group spheres+errors, NOT the render bounds: a cluster
// and the parents that replace it share the exact same (sphere, error) pair
// on the boundary between them, which is what makes the cut gap-free.
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
  bool Initialize(Device& device, Format color_format, Format depth_format);
  void Destroy(Device& device);

  // Builds the full cluster DAG from the mesh's lod 0 and uploads it.
  void Upload(Device& device, const asset::Mesh& mesh);
  bool active() const { return meshlet_count_ > 0; }
  u32 meshlet_count() const { return meshlet_count_; }
  u32 lod_count() const { return lod_count_; }
  u32 last_visible(u32 slot) const;

  // error_pixels: screen-space error budget (tau) in pixels.
  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  const Mat4& view_proj, const f32 planes[5][4], const Vec3& camera,
                  f32 proj_scale, f32 error_pixels, u32 slot);

 private:
  static constexpr u32 kFramesInFlight = 2;
  bool available_ = false;
  PipelineHandle pipeline_;

  GpuBuffer meshlets_;
  GpuBuffer meshlet_vertices_;
  GpuBuffer meshlet_triangles_;
  GpuBuffer vertices_;
  GpuBuffer counters_[kFramesInFlight];
  u32 meshlet_count_ = 0;
  u32 lod_count_ = 0;
};

}  // namespace rx::render

#endif  // RX_RENDER_VIRTUAL_GEOMETRY_H_
