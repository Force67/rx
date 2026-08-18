#ifndef RX_RENDER_RHI_VULKAN_INTEROP_H_
#define RX_RENDER_RHI_VULKAN_INTEROP_H_

// Vulkan escape hatch for modules that integrate api-specific SDKs (NRD, DLSS,
// FSR3, the imgui/ugui gui backend, thumbnailer). Everything here returns null
// handles when the device is not the Vulkan backend, so callers must feature-
// gate on GetVulkanHandles(...).device != VK_NULL_HANDLE. Pass code must NOT
// include this header; it exists so interop stays possible without leaking
// Vulkan back into the renderer's portable surface.

#include <volk.h>

#include <vk_mem_alloc.h>

#include "core/export.h"
#include "render/rhi/command_list.h"
#include "render/rhi/device.h"

namespace rx::render {

struct VulkanHandles {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue graphics_queue = VK_NULL_HANDLE;
  u32 graphics_family = 0;
  VmaAllocator allocator = nullptr;
};

// Null-filled when `device` is not the Vulkan backend.
RX_RENDER_EXPORT VulkanHandles GetVulkanHandles(Device& device);

// Frames-in-flight the renderer cycles (== the number of distinct frame slots a
// SceneHookContext::frame_slot can take). An app sizing per-frame resources for
// its own GPU passes at init time uses this instead of hardcoding a count.
// Returns 0 on the null backend.
RX_RENDER_EXPORT u32 GetVulkanFramesInFlight(Device& device);

// The command list's VkCommandBuffer, or null on other backends.
inline VkCommandBuffer GetVkCommandBuffer(CommandList& cmd) {
  return static_cast<VkCommandBuffer>(cmd.native_handle());
}

// Commands Vulkan promoted into 1.2 / 1.3 core. rx drives adapters as low as
// 1.1, where only the KHR original exists and volk leaves the core symbol null,
// so every call goes through this table instead of the core name. Filled once
// at device creation from the version the device was actually created at; all
// null before that and on other backends.
struct VulkanEntryPoints {
  PFN_vkCmdBeginRendering cmd_begin_rendering = nullptr;
  PFN_vkCmdEndRendering cmd_end_rendering = nullptr;
  PFN_vkCmdPipelineBarrier2 cmd_pipeline_barrier2 = nullptr;
  PFN_vkQueueSubmit2 queue_submit2 = nullptr;
  PFN_vkGetBufferDeviceAddress get_buffer_device_address = nullptr;
  PFN_vkCmdDrawIndirectCount cmd_draw_indirect_count = nullptr;
  PFN_vkCmdDrawIndexedIndirectCount cmd_draw_indexed_indirect_count = nullptr;
};
RX_RENDER_EXPORT const VulkanEntryPoints& VulkanApi();

// Raw handles behind rhi resources, for handing to api-specific SDKs.
RX_RENDER_EXPORT VkImage GetVkImage(const GpuImage& image);
RX_RENDER_EXPORT VkImageView GetVkImageView(TextureView view);
RX_RENDER_EXPORT VkSampler GetVkSampler(SamplerHandle sampler);
RX_RENDER_EXPORT VkBuffer GetVkBuffer(const GpuBuffer& buffer);
RX_RENDER_EXPORT VkAccelerationStructureKHR GetVkAccelStruct(AccelStructHandle accel);
RX_RENDER_EXPORT VkFormat GetVkFormat(Format format);

}  // namespace rx::render

#endif  // RX_RENDER_RHI_VULKAN_INTEROP_H_
