// The vk backend wires this FrameInterpolationSwapchain entry point into its
// interface table unconditionally. This build is upscale-only (no frame
// generation, the swapchain stays the engine's), so a failing stub replaces
// the heavy FrameInterpolationSwapchainVK.cpp.
#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>

FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*) {
  return FFX_ERROR_INVALID_ARGUMENT;
}
