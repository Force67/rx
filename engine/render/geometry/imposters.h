#ifndef RX_RENDER_IMPOSTERS_H_
#define RX_RENDER_IMPOSTERS_H_

// Octahedral imposters for distant foliage: a mesh is baked once into a
// hemi-octahedral atlas of views (albedo+coverage and world-normal per cell)
// and far instances then draw as single camera-facing quads that pick the
// atlas cell nearest their view direction - thousands of distant trees for
// two triangles each. Near instances stay real meshes; the swap distance is
// content's choice (the demo spawns real meshes inside the imposter ring).
//
// Several meshes share one atlas, tiled kMeshGrid x kMeshGrid, so a forest of
// species draws in one instanced call: an instance names its mesh by the index
// Bake returned. The bake samples each submesh's base-colour map and applies
// its alpha cutoff, because the meshes this replaces are alpha-masked cutouts
// whose leaf shape lives entirely in a texture's alpha channel - baking their
// vertex colours would produce untextured blobs.

#include <span>

#include "asset/mesh.h"
#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class ImposterPass {
 public:
  static constexpr u32 kGrid = 4;              // kGrid^2 hemi-octahedral views
  static constexpr u32 kCell = 128;            // texels per view
  static constexpr u32 kTile = kGrid * kCell;  // one baked mesh's views
  static constexpr u32 kMeshGrid = 4;          // mesh tiles per atlas axis
  static constexpr u32 kMaxMeshes = kMeshGrid * kMeshGrid;
  static constexpr u32 kAtlas = kMeshGrid * kTile;  // 2048: ~22 MB per atlas
  static constexpr u32 kNoMesh = ~0u;

  struct Instance {
    f32 position[3];
    f32 scale = 1.0f;
    u32 mesh = 0;       // the index Bake returned
    u32 pad[3] = {};    // std430: the shader's Instance is 32 bytes
  };

  // One submesh's albedo source for a bake. A null image bakes that submesh
  // from its vertex colours, which is what an untextured mesh wants.
  struct BakeMaterial {
    const GpuImage* base_color = nullptr;
    f32 alpha_cutoff = 0.0f;  // 0 = no cutout
  };

  bool Initialize(Device& device, Format color_format, Format depth_format);
  void Destroy(Device& device);
  bool active() const { return instance_count_ > 0 && mesh_count_ > 0; }

  // Bakes one mesh into the shared atlas and returns the index instances name
  // it by, or kNoMesh when the atlas is full or the mesh is empty. `materials`
  // is parallel to lod 0's submeshes; a short span leaves the rest untextured.
  u32 Bake(Device& device, const asset::Mesh& mesh,
           std::span<const BakeMaterial> materials = {});
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
  // Per baked mesh, mirrored into mesh_params_ for the draw shader.
  struct MeshParams {
    f32 radius = 1.0f;    // bounding radius the bake framed
    f32 center_y = 0.0f;  // bake-space center height
    f32 tile[2] = {0, 0}; // tile origin in atlas uv
  };

  void UploadMeshParams(Device& device);

  PipelineHandle bake_pipeline_;
  PipelineHandle draw_pipeline_;
  GpuImage albedo_atlas_;  // RGBA8: albedo + coverage alpha, kMaxMeshes tiles
  GpuImage normal_atlas_;  // RGBA8: bake-space world normal * 0.5 + 0.5
  GpuImage white_;         // 1x1, stands in for a submesh with no base-colour map
  SamplerHandle sampler_;
  GpuBuffer instances_;
  GpuBuffer mesh_params_;
  MeshParams meshes_[kMaxMeshes];
  u32 mesh_count_ = 0;
  u32 instance_count_ = 0;
};

}  // namespace rx::render

#endif  // RX_RENDER_IMPOSTERS_H_
