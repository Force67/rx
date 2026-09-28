#ifndef RX_RENDER_NGX_CONTEXT_H_
#define RX_RENDER_NGX_CONTEXT_H_

// Shared NVIDIA NGX (DLSS) runtime context, refcounted across the features
// that use it (the DLSS upscaler and the DLSS-D ray-reconstruction denoiser):
// NGX is initialized once per VkDevice and shut down when the last user
// releases it. Vulkan-backend only; compiled under RX_HAS_DLSS.

#include "foundation/build_config/types.h"

typedef struct NVSDK_NGX_Parameter NVSDK_NGX_Parameter;

namespace rx::gpu {
class Device;
}  // namespace rx::gpu

namespace rx::render {


namespace ngx {

// Initializes NGX for the device's Vulkan instance on first call, bumps a
// refcount otherwise. Returns false when the backend is not Vulkan or NGX
// init fails (callers degrade their feature gracefully).
bool Acquire(gpu::Device& device);
// Drops a reference; the last one shuts NGX down.
void Release();

// The NGX capability parameter block; valid while acquired.
NVSDK_NGX_Parameter* Capability();

}  // namespace ngx
}  // namespace rx::render

#endif  // RX_RENDER_NGX_CONTEXT_H_
