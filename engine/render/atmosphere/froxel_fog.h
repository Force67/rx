#ifndef RX_RENDER_FROXEL_FOG_H_
#define RX_RENDER_FROXEL_FOG_H_

// Unified froxel volumetric lighting: a camera-aligned 3D scattering volume
// (exponential slices to ~64m) lit by the sun and every clustered light -
// including their local shadow maps - then integrated front-to-back so the
// fog composite, translucents and particles all sample the same "everything
// in front of me" answer. Temporally jittered and reprojected.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class FroxelFog {
 public:
  static constexpr u32 kSizeX = 160;
  static constexpr u32 kSizeY = 96;
  static constexpr u32 kSizeZ = 64;
  static constexpr f32 kNear = 0.1f;  // must match the camera near plane
  static constexpr f32 kFar = 64.0f;

  struct Frame {
    Mat4 inv_view_proj;  // unjittered
    Mat4 prev_view_proj;
    Vec3 camera_pos;
    u32 frame_index = 0;
    Vec3 sun_direction;  // travel
    f32 anisotropy = 0.5f;
    Vec3 sun_color;  // premultiplied by intensity
    f32 ambient = 0.0f;
    f32 density = 0.015f;
    f32 height_falloff = 0.05f;
    f32 base_height = 0.0f;
    f32 cluster_params[4] = {0, 0, 0, 0};
    f32 screen_size[2] = {0, 0};
    bool csm_active = false;
    // Cluster + shadow inputs (dummies when a feature is off).
    GpuBuffer lights;
    GpuBuffer cluster_counts;
    GpuBuffer cluster_indices;
    GpuBuffer local_shadow_faces;
    TextureView local_shadow_atlas;
    GpuBuffer cascade_buffer;
    u64 cascade_size = 0;
    TextureView cascade_atlas;
    SamplerHandle comparison_sampler;
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(scatter_pipeline_); }

  // Records scatter + integrate + composite onto `lit`. cascade_atlas rides as
  // a graph handle so its transitions stay graph-owned; the local shadow
  // atlas is a persistent image the caller already moved to compute-read.
  void AddToGraph(RenderGraph& graph, ResourceHandle lit, ResourceHandle depth_export,
                  ResourceHandle cascade_atlas_handle, Extent2D extent, const Frame& frame);

  // Sampled by translucency passes after AddToGraph ran this frame.
  const GpuImage& integrated() const { return integrated_; }
  SamplerHandle volume_sampler() const { return sampler_; }

 private:
  PipelineHandle scatter_pipeline_;
  PipelineHandle integrate_pipeline_;
  PipelineHandle apply_pipeline_;
  GpuImage scatter_[2];  // temporal ping-pong
  GpuImage integrated_;
  SamplerHandle sampler_;
  GpuBuffer dummy_uniform_;
  bool volumes_initialized_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_FROXEL_FOG_H_
