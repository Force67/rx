#ifndef RX_RENDER_WATER_CAUSTICS_H_
#define RX_RENDER_WATER_CAUSTICS_H_

// Underwater caustics + wave shadows. Each frame a compute pass refracts a grid
// of sun rays through the water surface (the FFT displacement/normal maps when
// active, an analytic Gerstner field otherwise) onto a reference receiver plane
// a fixed depth below the rest height, and accumulates an ENERGY-CONSERVING
// caustic map: every surface photon carries unit energy and is splatted into
// the receiver texel it lands on, so the map's mean is 1 - convergent
// refraction brightens (R>1) and divergent refraction darkens (R<1), with no
// free energy. The result is a tiling world-space RG16F texture (R caustic
// density, G a soft wave-shadow term). mesh.ps/mesh_rt.ps sample it at env slot
// 34 (gated by kFrameFlagWaterCaustics) for surfaces below the water rest
// height, modulating direct sun. The map is stateless (rebuilt fully each
// frame), so no ping-pong or history is needed. See WATER_SHADING.md.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class WaterCaustics {
 public:
  static constexpr u32 kSize = 1024;   // texels per side (power of two: wrap masking).
                                       // 16 texels/m over the 64 m tile: the caustic web
                                       // stays a fine filament lattice instead of chunky
                                       // half-metre blobs when magnified at shallow depth.
  static constexpr f32 kTile = 64.0f;  // world tiling (m); mirrors the shader kTile / kCausticTile

  struct Params {
    Vec3 sun_travel{0, -1, 0};   // normalized sun travel direction (y < 0)
    f32 time = 0.0f;             // seconds, drives the Gerstner evaluation
    f32 rest_height = 0.0f;      // water rest plane y (m)
    f32 receiver_depth = 4.0f;   // reference caustic receiver depth below rest (m)
    bool fft_active = false;     // sample the ocean maps vs the Gerstner field
    TextureView ocean_displacement;  // valid when fft_active
    TextureView ocean_normal;        // valid when fft_active
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_) && static_cast<bool>(caustic_); }

  // Records the clear + scatter + resolve dispatches for this frame.
  void AddToGraph(RenderGraph& graph, const Params& params);

  // The caustic map written this frame (RG16F, kept in GENERAL), sampled by the
  // opaque scene pass through env slot 34.
  TextureView current_view() const { return caustic_.view; }

 private:
  PipelineHandle pipeline_;
  GpuImage caustic_;       // RG16F, kept in GENERAL
  GpuBuffer accum_;        // kSize*kSize uint fixed-point energy accumulation
  GpuImage dummy_ocean_;   // 1x1 stand-in bound when the FFT ocean is off
  SamplerHandle linear_wrap_;
};

}  // namespace rx::render

#endif  // RX_RENDER_WATER_CAUSTICS_H_
