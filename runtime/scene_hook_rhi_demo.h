#ifndef RX_RUNTIME_SCENE_HOOK_RHI_DEMO_H_
#define RX_RUNTIME_SCENE_HOOK_RHI_DEMO_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "render/core/renderer.h"
#include "render/rhi/resources.h"

namespace rx {

// Acceptance demo for the GPU-driven-renderer RHI surface (--demo scenehook-rhi).
// Unlike SceneHookDemo (which proves the raw-Vulkan escape hatch), this one is
// PURE RHI end to end: a 2D texture array uploaded per layer, a compute pass that
// fills a buffer-device-address instance arena AND writes the indirect draw args
// + count into BDA buffers, then a classic DrawIndirectCount (and, when the GPU
// has mesh shaders, a DrawMeshTasksIndirect) that samples the texture array,
// depth-interleaved with rx's own cube. A per-frame churned scratch buffer is
// retired through the frame-safe deferred-destruction path. Nothing here names a
// Vulkan/D3D12 type; it runs on whatever backend the scene hook fires on
// (Vulkan today).
class SceneHookRhiDemo {
 public:
  ~SceneHookRhiDemo();

  bool Init(render::Renderer& renderer);
  void Shutdown();
  void Emit(f32 dt, render::FrameView& view);
  bool ready() const { return ready_; }

 private:
  void Record(const render::SceneHookContext& ctx);
  bool CreateTextureArray();

  struct Slot {
    render::GpuBuffer instances;  // BDA instance arena (pos/color/layer)
    render::GpuBuffer args;       // indirect draw + mesh-task args
    render::GpuBuffer count;      // draw count (for DrawIndirectCount)
  };

  render::Renderer* renderer_ = nullptr;
  render::Device* device_ = nullptr;
  render::PipelineHandle cull_pipeline_{};
  render::PipelineHandle draw_pipeline_{};
  render::PipelineHandle mesh_pipeline_{};  // null when mesh shaders unavailable
  render::GpuImage tex_array_{};
  render::SamplerHandle sampler_{};
  base::Vector<Slot> slots_;  // one per frame-in-flight
  u32 instance_count_ = 12;
  u32 layer_count_ = 4;
  f32 time_ = 0;
  bool use_mesh_ = false;
  bool ready_ = false;
};

}  // namespace rx

#endif  // RX_RUNTIME_SCENE_HOOK_RHI_DEMO_H_
