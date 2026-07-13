#ifndef RX_RENDER_SHORE_WETTING_H_
#define RX_RENDER_SHORE_WETTING_H_

// Wave-driven shoreline wetting. Maintains a camera-following, world-space R16F
// field (0 dry .. 1 soaked) covering kExtent metres around the eye. Each frame
// a compute pass compares the water surface height (the same Gerstner field the
// water uses, or the FFT ocean displacement when active) against a terrain
// height at every texel: submerged texels latch to soaked, exposed texels dry
// out exponentially. mesh.ps samples the field (env slot 33, gated by
// kFrameFlagShoreWetting) to darken/sharpen wet opaque surfaces. The field
// ping-pongs and is resampled at the previous origin as the camera moves, so no
// toroidal bookkeeping is needed. See SHORELINE_WETTING.md.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class ShoreWetting {
 public:
  static constexpr u32 kResolution = 1024;   // mirrors kResolution in the shader
  static constexpr f32 kExtent = 128.0f;     // world metres the field covers

  struct Params {
    Vec3 camera_eye;
    f32 time = 0.0f;         // seconds, drives the Gerstner evaluation
    f32 dt = 0.0f;           // frame delta, drives the drying decay
    f32 drying_time = 28.0f; // seconds for a wet patch to fade back to dry
    f32 island[4] = {0, 0, 10, 1.5f};  // analytic beach: center xz, sigma, peak
    bool fft_active = false;           // sample ocean_displacement vs Gerstner
    TextureView ocean_displacement;    // valid when fft_active
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_) && static_cast<bool>(fields_[0]); }

  // Snap the field origin to the camera and advance the ping-pong. Call once
  // per frame, before FrameGlobals is uploaded and before AddToGraph.
  void BeginFrame(const Vec3& camera_eye);
  // Fills FrameGlobals::shore_field: origin xz, 1/extent, rest water height.
  void FieldParams(f32 out[4]) const;

  // Records the wetting compute for this frame using the origin from BeginFrame.
  void AddToGraph(RenderGraph& graph, const Params& params);

  // The field written this frame, sampled by the scene pass.
  TextureView current_view() const { return fields_[write_index_].view; }

 private:
  PipelineHandle pipeline_;
  GpuImage fields_[2];      // ping-pong R16F, both kept in GENERAL
  GpuImage dummy_ocean_;    // 1x1 stand-in bound when the FFT ocean is off
  SamplerHandle linear_clamp_;
  SamplerHandle linear_wrap_;
  u32 write_index_ = 0;
  u32 read_index_ = 1;
  f32 origin_[2] = {0, 0};       // this frame's field min-corner world xz
  f32 prev_origin_[2] = {0, 0};  // the origin the read field was written at
  bool have_prev_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_SHORE_WETTING_H_
