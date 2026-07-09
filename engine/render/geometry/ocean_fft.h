#ifndef RX_RENDER_OCEAN_FFT_H_
#define RX_RENDER_OCEAN_FFT_H_

// Tessendorf FFT ocean: a Phillips-spectrum height field evolved analytically
// in frequency space each frame and inverse-FFT'd on the GPU (shared-memory
// radix-2 per row/column) into tiling displacement + normal/foam maps that
// replace the four hand-tuned Gerstner waves. The water vertex/pixel shaders
// sample the maps through env slots 28/29 when kFrameFlagFftOcean is set;
// RX_FFT_OCEAN=0 falls back to Gerstner.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class OceanFft {
 public:
  static constexpr u32 kSize = 256;        // grid resolution (power of two)
  static constexpr f32 kPatchSize = 64.0f; // world meters per tile (mirrored in water_waves.hlsli)

  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(spectrum_pipeline_); }

  // Records spectrum -> IFFT -> maps for this frame's time.
  void AddToGraph(RenderGraph& graph, f32 time);

  TextureView displacement_view() const { return displacement_.view; }
  TextureView normal_foam_view() const { return normal_foam_.view; }

 private:
  PipelineHandle spectrum_pipeline_;
  PipelineHandle fft_pipeline_;
  PipelineHandle finalize_pipeline_;
  PipelineHandle normals_pipeline_;
  GpuImage h0_;            // RGBA32F: h0(k).re/.im, h0(-k).re/.im (CPU generated)
  GpuImage spectrum_[2];   // RGBA32F ping/pong: (h.re,h.im,dx.re,dx.im)
  GpuImage spectrum_z_[2]; // RGBA32F ping/pong: (dz.re,dz.im,0,0)
  GpuImage displacement_;  // RGBA16F world-space displacement
  GpuImage normal_foam_;   // RGBA16F normal.xyz + foam
};

}  // namespace rx::render

#endif  // RX_RENDER_OCEAN_FFT_H_
