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

class RayTracingContext;

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
    // Metres of clear air in front of the camera before the fog ramps in.
    // 0 = fog from the near plane.
    f32 start_distance = 0.0f;
    f32 cluster_params[4] = {0, 0, 0, 0};
    f32 screen_size[2] = {0, 0};
    bool csm_active = false;
    // Shadow the sun with one inline ray per froxel instead of the cascades.
    // The rt sun-shadow tier switches the cascades off, and an unshadowed sun
    // fills interiors with a flat glow instead of window shafts.
    bool ray_query_sun = false;
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

  // ray_query builds the second scatter pipeline that shadows the sun with an
  // inline ray; without it Frame::ray_query_sun is ignored and the pass stays
  // on the cascades.
  bool Initialize(Device& device, bool ray_query);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(scatter_pipeline_); }

  // Records scatter + integrate + composite onto `lit`. cascade_atlas rides as
  // a graph handle so its transitions stay graph-owned; the local shadow
  // atlas is a persistent image the caller already moved to compute-read.
  // `raytracing` may be null (no rt tier); it is only read when the frame asks
  // for the ray-query sun and the rt pipeline came up.
  void AddToGraph(RenderGraph& graph, ResourceHandle lit, ResourceHandle depth_export,
                  ResourceHandle cascade_atlas_handle, RayTracingContext* raytracing,
                  u32 tlas_slot, Extent2D extent, const Frame& frame);

  // Whether the inline-ray scatter variant was built. False on a device with no
  // ray query, or if that pipeline failed to compile.
  bool ray_query_available() const { return static_cast<bool>(scatter_pipeline_rt_); }

  // Sampled by translucency passes after AddToGraph ran this frame.
  const GpuImage& integrated() const { return integrated_; }
  SamplerHandle volume_sampler() const { return sampler_; }

 private:
  PipelineHandle scatter_pipeline_;
  PipelineHandle scatter_pipeline_rt_;
  PipelineHandle integrate_pipeline_;
  PipelineHandle apply_pipeline_;
  GpuImage scatter_[2];  // temporal ping-pong
  GpuImage integrated_;
  SamplerHandle sampler_;
  GpuBuffer dummy_uniform_;
  GpuBuffer camera_[2];  // scatter matrices, too big for the push block
  bool volumes_initialized_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_FROXEL_FOG_H_
