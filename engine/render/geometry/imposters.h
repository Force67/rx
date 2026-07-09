#ifndef RX_RENDER_IMPOSTERS_H_
#define RX_RENDER_IMPOSTERS_H_

// Octahedral imposters for distant foliage: a mesh is baked once into a
// hemi-octahedral atlas of views (albedo+coverage and world-normal per cell)
// and far instances then draw as single camera-facing quads that pick the
// atlas cell nearest their view direction - thousands of distant trees for
// two triangles each. Near instances stay real meshes; the swap distance is
// content's choice (the demo spawns real meshes inside the imposter ring).

#include "asset/mesh.h"
#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class ImposterPass {
 public:
  static constexpr u32 kGrid = 4;       // kGrid^2 hemi-octahedral views
  static constexpr u32 kCell = 128;     // texels per view
  static constexpr u32 kAtlas = kGrid * kCell;

  struct Instance {
    f32 position[3];
    f32 scale = 1.0f;
  };

  bool Initialize(Device& device, Format color_format, Format depth_format);
  void Destroy(Device& device);
  bool active() const { return instance_count_ > 0 && baked_; }

  // Renders the mesh (vertex colors as albedo) into the octahedral atlas.
  void Bake(Device& device, const asset::Mesh& mesh);
  void SetInstances(Device& device, std::span<const Instance> instances);

  struct Frame {
    Mat4 view_proj;
    Vec3 camera_pos;
    Vec3 sun_direction;  // travel
    f32 sun_intensity = 3.0f;
    Vec3 sun_color{1, 1, 1};
    f32 ambient = 0.25f;
  };

  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  Extent2D extent, const Frame& frame);

 private:
  PipelineHandle bake_pipeline_;
  PipelineHandle draw_pipeline_;
  GpuImage albedo_atlas_;  // RGBA8: vertex-color albedo + coverage alpha
  GpuImage normal_atlas_;  // RGBA8: bake-space world normal * 0.5 + 0.5
  GpuImage bake_depth_;
  SamplerHandle sampler_;
  GpuBuffer instances_;
  u32 instance_count_ = 0;
  f32 mesh_radius_ = 1.0f;
  f32 mesh_center_y_ = 0.0f;
  bool baked_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_IMPOSTERS_H_
