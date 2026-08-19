#ifndef RX_RENDER_REFERENCE_COMPARE_H_
#define RX_RENDER_REFERENCE_COMPARE_H_

#include <string>

#include "core/types.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

// Reference comparison against calibrated photography.
//
// This exists because the character work's first investment is the validation
// framework, not the shader. A renderer you cannot measure against reality is
// a renderer you tune by argument. The pass runs on the SCENE-LINEAR image
// immediately before exposure and tonemap, so the reference and the render
// travel the same colour path and a difference is a material difference.
//
// It also accumulates a per-region error metric, which is what lets the lookdev
// tool's fitting be a measurement instead of a preference.
class ReferenceCompare {
 public:
  enum class Mode : u8 {
    kOff,
    kSideBySide,          // render left, reference right, both whole
    kWipe,                // sliding split
    kLinearDifference,    // |render - reference| in scene-linear
    kDisplayDifference,   // both through the frame's exposure + tonemap first
    kReferenceOnly,
  };

  // Regions the error metric is bucketed into. They come from a mask texture's
  // four channels; fitting one material against a mask that mixes them
  // converges on none of them.
  enum class Region : u8 { kAll, kSkin, kEyes, kLips, kTeeth };

  struct Settings {
    Mode mode = Mode::kOff;
    Region region = Region::kAll;
    f32 split = 0.5f;
    f32 difference_gain = 8.0f;
    f32 reference_exposure = 1.0f;  // linear multiplier on the reference
    f32 uv_scale[2] = {1.0f, 1.0f};  // reference alignment
    f32 uv_offset[2] = {0.0f, 0.0f};
    bool collect_stats = false;
    // The exposure the display-referred modes apply before tonemapping. The
    // lookdev rig FREEZES exposure (that is the point of a calibrated bench),
    // so this is authored rather than read back from auto-exposure - which
    // would make the metric depend on what else is on screen.
    f32 exposure_scale = 1.0f;
  };

  // Per-region readback of the accumulated error, one frame behind (the
  // readback is deliberately not stalled - a fitting loop wants throughput,
  // and a one-frame-old metric of a static rig is the same metric).
  struct Stats {
    f64 mean_squared_error = 0.0;
    f64 mean_absolute_error = 0.0;
    f64 mean_reference_luma = 0.0;
    f64 coverage = 0.0;  // masked pixel count
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // Loads a scene-linear reference. .hdr / .exr-style float sources load
  // as-is; 8-bit sources are de-gamma'd to linear on the way in. Returns false
  // and keeps the previous reference on a read error.
  bool LoadReference(Device& device, const std::string& path);
  // Loads the four-channel region mask (r skin, g eyes, b lips, a teeth).
  bool LoadRegionMask(Device& device, const std::string& path);
  bool has_reference() const { return static_cast<bool>(reference_); }

  Settings& settings() { return settings_; }
  const Settings& settings() const { return settings_; }

  // Returns the image to hand to the rest of post. Passing through unchanged
  // when the mode is off costs one copy, so the caller should skip the call.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle scene_color, Extent2D extent,
                            u32 tonemap_op);

  // Reads back last frame's accumulated metric for one region.
  Stats stats(Region region) const;

 private:
  Settings settings_;
  PipelineHandle pipeline_;
  SamplerHandle sampler_;
  GpuImage reference_;
  GpuImage region_mask_;
  GpuImage white_mask_;  // 1x1 all-channels-1, so "no mask loaded" means "all"
  GpuBuffer stats_buffer_;
  Device* device_ = nullptr;
};

}  // namespace rx::render

#endif  // RX_RENDER_REFERENCE_COMPARE_H_
