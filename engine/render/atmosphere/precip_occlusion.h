#ifndef RX_RENDER_PRECIP_OCCLUSION_H_
#define RX_RENDER_PRECIP_OCCLUSION_H_

#include <functional>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

// Top-down "what can see the sky" depth map gating every precipitation effect:
// no rain or snow under bridges and roofs, splashes land ON the roof, wetness
// and snow cover stop at the drip line. A persistent D16 map re-rendered
// (depth-only, reusing ShadowPass's caster pipelines) only when the camera
// crosses a coarse anchor cell or on a slow refresh cadence, so the steady
// state costs nothing.
//
// World -> map convention (consumed by precip_volume.vs / precip_splash.vs /
// surface_weather.cs; params() packs it):
//   uv         = ((world.xz - center.xz) / kHalfExtent) * 0.5 + 0.5
//   occluder_y = top_y - sample * y_range()
// A point is sky-visible when world.y >= occluder_y - bias.
class PrecipOcclusion {
 public:
  static constexpr u32 kResolution = 512;
  // Texel = 2 * 102.4 / 512 = 0.4 m, an exact divisor of the 8 m anchor cell,
  // so a re-anchored map lands on the same world texel grid and never shimmers.
  static constexpr f32 kHalfExtent = 102.4f;   // metres of cover around the anchor
  static constexpr f32 kHalfHeight = 180.0f;   // generous vertical range around the eye
  static constexpr f32 kAnchorCell = 8.0f;     // re-render when the eye crosses a cell
  static constexpr u32 kRefreshFrames = 16;    // steady-state cadence (dynamic casters)
  // Matches ShadowPass::kAtlasFormat so its depth-only caster pipelines render
  // here unchanged.
  static constexpr Format kFormat = Format::kD16Unorm;

  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(map_); }

  // Snap the anchor to the coarse cell and decide whether this frame re-renders.
  // Call once per frame before AddToGraph / params().
  void BeginFrame(const Vec3& eye, u32 frame_index);

  // Fills {center_x, center_z, 1/kHalfExtent, top_y} for the shader-side
  // transform above.
  void Params(f32 out[4]) const;
  static constexpr f32 y_range() { return 2.0f * kHalfHeight; }

  TextureView view() const { return map_.view; }
  SamplerHandle sampler() const { return sampler_; }

  // Depth-only render of the frame's opaque draws when the anchor moved or the
  // refresh cadence hit; no-op otherwise. draw() receives the top-down
  // view-proj and records the casters, same contract as ShadowPass::Render's
  // callback (the caller binds the depth-only pipelines and pushes matrices).
  void AddToGraph(RenderGraph& graph,
                  const std::function<void(CommandList&, const Mat4&)>& draw);

 private:
  GpuImage map_;            // persistent D16, parked shader-read between renders
  SamplerHandle sampler_;   // linear clamp (consumers want soft dilated edges)
  f32 center_[3] = {0, 0, 0};  // anchor, quantized to kAnchorCell
  bool dirty_ = true;          // anchor moved / cadence hit -> render this frame
  bool rendered_ = false;      // image holds a render (first use transitions from undefined)
};

}  // namespace rx::render

#endif  // RX_RENDER_PRECIP_OCCLUSION_H_
