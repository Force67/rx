#ifndef RX_RUNTIME_SCENE_HOOK_DEMO_H_
#define RX_RUNTIME_SCENE_HOOK_DEMO_H_

#include <volk.h>

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/renderer.h"
#include "render/rhi/resources.h"

namespace rx {

// Acceptance demo for rx's app scene-pass hooks (--demo scenehook): a game's own
// GPU-driven pipeline built entirely through the Vulkan interop escape hatch (no
// rhi pipeline objects). Each frame it records, into rx's scene at the opaque
// phase, a compute "placement/cull" dispatch that fills a buffer-device-address
// arena, then an instanced box draw reading that arena - depth-interleaved with
// a normal rx-drawn cube. Nothing here touches rx internals beyond the public
// SceneHookContext + vulkan_interop unwrappers.
class SceneHookDemo {
 public:
  ~SceneHookDemo();

  // Builds the raw compute + graphics pipelines and the per-slot BDA arenas.
  // Vulkan backend only; returns false (and stays inert) otherwise.
  bool Init(render::Renderer& renderer);
  void Shutdown();

  // Installs the scene_opaque hook on this frame's view and advances animation.
  void Emit(f32 dt, render::FrameView& view);

  bool ready() const { return ready_; }

 private:
  void Record(const render::SceneHookContext& ctx);

  render::Renderer* renderer_ = nullptr;
  render::Device* device_ = nullptr;
  VkDevice vk_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline compute_ = VK_NULL_HANDLE;
  VkPipeline graphics_ = VK_NULL_HANDLE;
  base::Vector<render::GpuBuffer> arenas_;  // one BDA arena per frame-in-flight
  u32 instance_count_ = 12;
  f32 time_ = 0;
  bool ready_ = false;
};

}  // namespace rx

#endif  // RX_RUNTIME_SCENE_HOOK_DEMO_H_
