#ifndef RX_RENDER_SHADER_UTIL_H_
#define RX_RENDER_SHADER_UTIL_H_

// Vulkan-backend-only utility (compiled under RX_RHI_VULKAN). Pass
// code never needs this; it exists for interop modules that build their own
// Vulkan pipelines (NRD, the runtime gui backend, the thumbnailer).

#include <volk.h>

#include <cstddef>

#include "core/export.h"

namespace rx::render {

// Wraps an embedded spirv blob. The embedded arrays are byte aligned,
// spirv wants words, so this copies. Returns VK_NULL_HANDLE on failure.
RX_RENDER_EXPORT VkShaderModule CreateShaderModule(VkDevice device, const unsigned char* code,
                                                   size_t size);

}  // namespace rx::render

#endif  // RX_RENDER_SHADER_UTIL_H_
