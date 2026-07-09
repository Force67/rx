#ifndef RX_RENDER_DENOISER_RR_H_
#define RX_RENDER_DENOISER_RR_H_

#if defined(RX_HAS_DLSS)

// DLSS Ray Reconstruction (DLSS-D): a learned denoiser that replaces the
// in-tree SVGF chain for the recon path tracer when the NGX runtime and its
// dlssd snippet are present. Consumes the noisy composed radiance plus the
// gbuffer guides (diffuse/specular albedo, world normals with packed
// roughness, depth, motion) and produces the final denoised color. Runs at
// native resolution (denoise-only); the engine's upscalers stay separate.
// Vulkan-backend only, compiled under RX_HAS_DLSS.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/types.h"

typedef struct NVSDK_NGX_Handle NVSDK_NGX_Handle;
typedef struct NVSDK_NGX_Parameter NVSDK_NGX_Parameter;

namespace rx::render {

class Device;

class RrDenoiser {
 public:
  struct Frame {
    Mat4 world_to_view;
    Mat4 view_to_clip;
    f32 frame_delta_ms = 16.6f;
    bool reset = false;
  };

  // Guide inputs, all render-resolution graph handles from the recon gbuffer.
  struct Inputs {
    ResourceHandle color = kInvalidResource;            // composed noisy radiance
    ResourceHandle depth = kInvalidResource;            // reversed-inf-z device depth
    ResourceHandle motion = kInvalidResource;           // uv-space current->previous
    ResourceHandle normals_rough = kInvalidResource;    // xyz world normal, w roughness
    ResourceHandle diffuse_albedo = kInvalidResource;
    ResourceHandle specular_albedo = kInvalidResource;
  };

  bool Initialize(Device& device, Extent2D extent);
  void Resize(Device& device, Extent2D extent);
  void Destroy(Device& device);
  bool available() const { return handle_ != nullptr; }

  // Adds the evaluate pass writing the denoised color into output.
  void AddToGraph(RenderGraph& graph, const Inputs& inputs, ResourceHandle output,
                  const Frame& frame);

 private:
  bool CreateFeature(Device& device, Extent2D extent);
  void ReleaseFeature();

  Extent2D extent_{};
  bool ngx_acquired_ = false;
  NVSDK_NGX_Handle* handle_ = nullptr;
  NVSDK_NGX_Parameter* params_ = nullptr;
};

}  // namespace rx::render

#endif  // RX_HAS_DLSS

#endif  // RX_RENDER_DENOISER_RR_H_
