#ifndef RX_RENDER_CLOUDSCAPE_H_
#define RX_RENDER_CLOUDSCAPE_H_

#include "core/math.h"
#include "render/atmosphere/cloudscape_textures.h"
#include "render/atmosphere/cloudscape_types.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rx::render {

class Device;

// The opt-in textured cloud model (RenderSettings::cloudscape): a spherical
// cloud shell driven by a world-space weather map and baked tileable noise,
// marched at half resolution with a two-mode (cheap/full) walk, and amortized
// over a 16-frame refresh cycle with distance-based reprojection. Replaces the
// always-on procedural Clouds pass when enabled; the weather layer feeds it
// through CloudscapeControls.
class Cloudscape {
public:
  struct Frame {
    Mat4 inv_view_proj;
    Mat4 prev_view_proj;
    Vec3 camera_pos;
    f32 time = 0.0f;
    u32 frame_index = 0;
    Vec3 sun_direction;
    f32 sun_intensity = 4.0f;
    Vec3 sun_color{1, 1, 1};
    f32 ambient = 1.0f; // scales the sky-ambient floor
    // Lightning flash. Applied in the full-res composite, NOT the march: the
    // flash is far faster than the refresh cycle, and boosting the amortized
    // march would print the refresh grid and contaminate history. `flash` is
    // the distance-damped global level; a distant active strike additionally
    // glows directionally toward its azimuth using the raw envelope.
    f32 flash = 0.0f;
    f32 flash_raw = 0.0f; // undamped strike envelope
    Vec3 strike_pos{0, 0, 0};
    bool strike_active = false;
    u32 steps = 48; // potential full samples toward the zenith
    CloudscapeControls controls;
  };

  bool Initialize(Device &device);
  void Destroy(Device &device);

  // Appends the noise bakes / weather-map refresh, the half-res march and the
  // full-res composite. Returns the composited color.
  ResourceHandle AddToGraph(RenderGraph &graph, ResourceHandle color,
                            ResourceHandle depth, Extent2D extent,
                            const Frame &frame);

  // Ground shadows from the same density field the march renders: darkens the
  // denoised sun-shadow buffer where the deck actually occludes the sun.
  // Replaces the legacy procedural cloud_shadow pass while the model is on.
  void AddShadowToGraph(RenderGraph &graph, ResourceHandle sun_shadow,
                        ResourceHandle depth, Extent2D extent,
                        const Frame &frame, f32 strength);

  bool available() const { return textures_.ready(); }

private:
  void EnsureBuffers(Device &device, Extent2D half);
  void ReleaseBuffers(Device &device);

  Device *device_ = nullptr;
  CloudscapeTextures textures_;
  PipelineHandle march_pipeline_;
  PipelineHandle apply_pipeline_;
  PipelineHandle shadow_pipeline_;

  // Persistent half-res ping-pong: (scatter, transmittance) + marched mean
  // cloud distance, reprojected across the refresh cycle.
  GpuImage cloud_[2];
  GpuImage dist_[2];
  ResourceState cloud_state_[2] = {ResourceState::kUndefined,
                                   ResourceState::kUndefined};
  ResourceState dist_state_[2] = {ResourceState::kUndefined,
                                  ResourceState::kUndefined};
  Extent2D half_extent_{};
  u32 slot_ = 0;
  bool history_valid_ = false;
};

} // namespace rx::render

#endif // RX_RENDER_CLOUDSCAPE_H_
