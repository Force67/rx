#ifndef RX_RENDER_FLUID_SIM_H_
#define RX_RENDER_FLUID_SIM_H_

// Optional GPU heightfield fluid solver (flowing water + lava) over one
// world-anchored square domain. Classic virtual-pipes shallow water (Mei et al.
// 2007) with the Chentanez & Muller stability kit (volume clamp, velocity
// clamp, edge-overshoot damp) and a MAGFLOW-style lava layer (temperature ->
// mobility/yield, donor-cell heat advection, cooling, solidification into a
// crust that becomes bed for both fluids). Default off; see FLUID_SIM.md.
//
// Per substep the solver runs four compute dispatches over resolution^2 cells:
// lava flux, lava integrate (+thermal +solidify), water flux, water integrate
// (+quench). The bed is CPU-authoritative (the game owns terrain + stamped
// obstacle heights and re-uploads on bed_version change); the two fluids share
// the domain, lava solving first so water sees B + C + d_lava as its bed. The
// solver core produces the fields; a separate surface renderer draws them.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

// One source the sim injects this frame: a spring/vent/drain adding depth (or
// removing it, negative rate) within a radius. Bounded per-frame list.
struct FluidSource {
  Vec3 position{};            // world; XZ used
  f32 radius = 1.0f;          // meters
  f32 rate = 0.0f;            // meters of depth per second within the radius
  u32 fluid = 0;              // 0 = water, 1 = lava
  f32 temperature = 1200.0f;  // lava sources only (degrees C)
};

// The active domain this frame. World-anchored, square, power-of-two grid. The
// bed pointer is only read during AddToGraph (copied synchronously into a
// staging buffer), so the caller may free it after the call returns.
struct FluidDomainDesc {
  f32 origin[2] = {0, 0};              // world XZ of the min corner
  f32 extent = 128.0f;                 // meters (square)
  u32 resolution = 512;                // cells per side (pow2, <= 1024)
  const f32* bed = nullptr;            // resolution^2 row-major heights (world Y); required
  const f32* initial_water = nullptr;  // optional resolution^2 initial water depth
  u64 bed_version = 0;                 // bump to re-upload bed (obstacle added/removed)
  f32 ambient_temperature = 20.0f;
};

class FluidSim {
 public:
  static constexpr u32 kMaxSources = 64;
  static constexpr u32 kMaxResolution = 1024;
  static constexpr f32 kSubstepDt = 1.0f / 120.0f;  // fixed, deterministic
  static constexpr u32 kMaxSubsteps = 4;            // per frame cap

  bool Initialize(Device& device);  // pipeline only; images on first Configure
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_); }

  struct UpdateParams {
    const FluidDomainDesc* domain = nullptr;  // null => sim idle this frame
    f32 dt = 0;
    u32 frame_slot = 0;
    const FluidSource* sources = nullptr;
    u32 source_count = 0;
  };

  // (Re)configures on desc/resolution change, uploads bed when bed_version
  // changes (initial_water applies only on (re)configure), then records the
  // substep dispatches. No-op when domain is null or the pipeline failed.
  void AddToGraph(RenderGraph& graph, const UpdateParams& params);

  bool active() const { return configured_ && have_domain_; }  // last AddToGraph had a domain
  TextureView bed_view() const { return bed_.view; }           // R32F
  TextureView state_view() const { return state_[read_].view; }  // RGBA32F: dw, dl, T, C
  TextureView velocity_view() const { return velocity_.view; }   // RGBA16F: water uv, lava uv
  const FluidDomainDesc& domain() const { return domain_; }
  const GpuBuffer& params_buffer(u32 frame_slot) const {
    return params_[frame_slot % Device::kMaxFramesInFlight];
  }

 private:
  friend struct FluidSimProbe;  // readback seam for test/fluid_sim_test.cc

  // Mirrors the shader's GpuParams CB the surface renderer maps world XZ with.
  struct GpuParams {
    f32 origin[2];
    f32 extent;
    f32 texel;       // cell size l (meters)
    f32 resolution;  // as float, for the shader's uv math
    f32 pad[3];
  };

  // Recreate the domain images and upload the initial bed/water. Called on the
  // first frame with a domain and whenever resolution/extent/origin change.
  void Configure(Device& device, const FluidDomainDesc& desc);
  void UploadBed(Device& device, const FluidDomainDesc& desc);
  void DestroyImages(Device& device);

  Device* device_ = nullptr;  // remembered so AddToGraph can (re)configure
  PipelineHandle pipeline_;
  GpuImage bed_;              // R32F static terrain + obstacles
  GpuImage state_[2];        // ping-pong RGBA32F: r=dw g=dl b=T a=C
  GpuImage flux_water_;      // RGBA32F outflow, updated in place
  GpuImage flux_lava_;       // RGBA32F outflow, updated in place
  GpuImage velocity_;        // RGBA16F: xy water uv, zw lava uv
  GpuBuffer params_[Device::kMaxFramesInFlight];
  GpuBuffer sources_[Device::kMaxFramesInFlight];

  FluidDomainDesc domain_{};  // last configured domain (bed ptr not retained)
  u64 bed_version_ = ~0ull;   // forces the first upload
  u32 read_ = 0;              // authoritative state slot (see AddToGraph)
  f32 accum_ = 0;             // substep remainder carried across frames
  bool configured_ = false;
  bool have_domain_ = false;  // last AddToGraph was handed a domain
};

// Readback seam for test/fluid_sim_test.cc: exposes the solver's output images
// (which the public API only surfaces as sampled views) so a test can
// ReadbackImage them. Not part of the renderer/demo API.
struct FluidSimProbe {
  static const GpuImage& state(const FluidSim& sim);
  static const GpuImage& velocity(const FluidSim& sim);
};

}  // namespace rx::render

#endif  // RX_RENDER_FLUID_SIM_H_
