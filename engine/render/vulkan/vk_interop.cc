#include "render/rhi/vulkan_interop.h"

#include "render/vulkan/vk_backend.h"

namespace rx::render {
namespace {

VulkanEntryPoints& EntryPoints() {
  static VulkanEntryPoints points;
  return points;
}

}  // namespace

const VulkanEntryPoints& VulkanApi() { return EntryPoints(); }

namespace vk {

void LoadVulkanEntryPoints(u32 api_version) {
  // The core symbol is only picked when the device was created at or above the
  // version that promoted it: volk resolves both spellings on a 1.3 driver, so
  // testing the pointer would silently take the core path even when rx asked
  // for a lower version.
  const bool core12 = api_version >= VK_API_VERSION_1_2;
  const bool core13 = api_version >= VK_API_VERSION_1_3;
  VulkanEntryPoints& points = EntryPoints();
  points.cmd_begin_rendering = core13 ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;
  points.cmd_end_rendering = core13 ? vkCmdEndRendering : vkCmdEndRenderingKHR;
  points.cmd_pipeline_barrier2 = core13 ? vkCmdPipelineBarrier2 : vkCmdPipelineBarrier2KHR;
  points.queue_submit2 = core13 ? vkQueueSubmit2 : vkQueueSubmit2KHR;
  points.get_buffer_device_address =
      core12 ? vkGetBufferDeviceAddress : vkGetBufferDeviceAddressKHR;
  points.cmd_draw_indirect_count = core12 ? vkCmdDrawIndirectCount : vkCmdDrawIndirectCountKHR;
  points.cmd_draw_indexed_indirect_count =
      core12 ? vkCmdDrawIndexedIndirectCount : vkCmdDrawIndexedIndirectCountKHR;
}

}  // namespace vk

VulkanHandles GetVulkanHandles(Device& device) {
  if (device.caps().backend != Backend::kVulkan) return {};
  auto& vk_device = static_cast<vk::VulkanDevice&>(device);
  return {.instance = vk_device.instance(),
          .physical_device = vk_device.physical_device(),
          .device = vk_device.device(),
          .graphics_queue = vk_device.graphics_queue(),
          .graphics_family = vk_device.graphics_family(),
          .allocator = vk_device.allocator()};
}

u32 GetVulkanFramesInFlight(Device& device) {
  if (device.caps().backend != Backend::kVulkan) return 0;
  return Device::kMaxFramesInFlight;
}

VkImage GetVkImage(const GpuImage& image) {
  return image.handle ? vk::Rec(image.handle)->image : VK_NULL_HANDLE;
}

VkImageView GetVkImageView(TextureView view) { return vk::View(view); }

VkSampler GetVkSampler(SamplerHandle sampler) { return vk::SamplerOf(sampler); }

VkBuffer GetVkBuffer(const GpuBuffer& buffer) {
  return buffer.handle ? vk::Rec(buffer.handle)->buffer : VK_NULL_HANDLE;
}

VkAccelerationStructureKHR GetVkAccelStruct(AccelStructHandle accel) {
  return accel ? vk::Rec(accel)->accel : VK_NULL_HANDLE;
}

VkFormat GetVkFormat(Format format) { return vk::ToVkFormat(format); }

}  // namespace rx::render
