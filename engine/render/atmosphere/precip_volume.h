#ifndef RX_RENDER_PRECIP_VOLUME_H_
#define RX_RENDER_PRECIP_VOLUME_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class Device;
class RayTracingContext;

// True volumetric precipitation: stateless procedural GPU particles (position
// is a pure function of instance id + time + weather, no sim buffers) drawn
// pre-resolve into the lit target with motion vectors and TAA jitter. Rain is
// velocity-stretched translucent streaks, snow soft swaying flakes; both are
// gated by the PrecipOcclusion sky map so nothing falls under roofs, and rain
// adds stateless impact splashes whose ground height also comes from that map
// (roofs and bridges splash too). An optional ray-query vertex variant shoots
// one sun ray per particle so rain sheets darken in shadowed alleys.
class PrecipVolume {
 public:
  static constexpr u32 kMaxRain = 131072;
  static constexpr u32 kMaxSnow = 98304;
  static constexpr u32 kMaxSplashes = 24576;  // x2 instances (ring + crown)

  struct Frame {
    Mat4 view_proj;
    Mat4 prev_view_proj;
    Vec3 cam_right;
    Vec3 cam_up;
    Vec3 cam_pos;
    Vec3 sun_direction;  // travel direction
    Vec3 sun_color;
    f32 sun_intensity = 4.0f;
    f32 ambient = 0.06f;
    f32 time = 0.0f;
    f32 dt = 1.0f / 60.0f;      // rewinds the stateless motion vector
    f32 intensity = 0.0f;       // precipitation 0..1
    bool snow = false;
    f32 wind[2] = {0, 0};       // wind velocity on xz, m/s
    f32 gustiness = 0.3f;
    f32 lightning = 0.0f;
    f32 jitter[2] = {0, 0};
    // Sky-occlusion map (PrecipOcclusion::Params / y_range / view / sampler).
    f32 occl[4] = {0, 0, 0, 0};
    f32 occl_range = 0.0f;
    TextureView occlusion;
    SamplerHandle occlusion_sampler;
    // Froxel fog transmittance (dims particles with the fog in front).
    bool froxel_enabled = false;
    TextureView froxel_volume;
    SamplerHandle froxel_sampler;
    // Per-particle sun shadow rays; effective only when the rt pipeline exists.
    bool rt_shadows = false;
  };

  // ray_query builds the additional rt vertex variant.
  bool Initialize(Device& device, Format color_format, bool ray_query);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_); }

  // Draws the rain/snow volume (and splashes when raining) over the lit scene:
  // color is the lit target, depth the prepass depth export (soft fade +
  // occlusion), motion the velocity target. raytracing/tlas_slot feed the rt
  // variant and may be null/ignored when frame.rt_shadows is off.
  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  ResourceHandle motion, RayTracingContext* raytracing, u32 tlas_slot,
                  const Frame& frame);

 private:
  PipelineHandle pipeline_;         // rain/snow, unshadowed vertex stage
  PipelineHandle pipeline_rt_;      // + per-particle sun ray query in the vs
  PipelineHandle splash_pipeline_;  // ripple rings + crown flashes
};

}  // namespace rx::render

#endif  // RX_RENDER_PRECIP_VOLUME_H_
