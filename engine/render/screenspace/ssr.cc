#include "render/screenspace/ssr.h"

#include <cstring>

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/ssr_cs_hlsl.h"

namespace rx::render {
namespace {

// The two matrices on their own are the entire 128 bytes vulkan guarantees for
// a push block, so they ride in a per-frame uniform buffer and the push keeps
// the scalars.
struct SsrCamera {
  Mat4 view_proj;
  Mat4 inv_view_proj;
};

struct SsrPush {
  f32 camera_pos[4];
  f32 inv_size[2];
  f32 intensity;
  f32 max_distance;
  f32 thickness;
  f32 frame_index;
  u32 step_count;
  u32 pad;
};

}  // namespace

bool SsrPass::Initialize(Device& device) {
  // 0: output color (storage), 1: depth, 2: normals, 3: scene color (all
  // sampled), 4: SsrCamera.
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_ssr_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kUniformBuffer}}}},
      .push_constant_size = PushSize<SsrPush>(),
      .debug_name = "ssr",
  });
  if (!pipeline_) {
    RX_ERROR("ssr pipeline creation failed");
    return false;
  }
  // One per in-flight frame: the pass rewrites it while the previous frame may
  // still be reading its own copy.
  for (GpuBuffer& camera : camera_) {
    camera = device.CreateBuffer(sizeof(SsrCamera), kBufferUsageUniform, true);
    if (!camera.mapped) return false;
  }
  return true;
}

void SsrPass::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (GpuBuffer& camera : camera_) {
    if (camera) device.DestroyBuffer(camera);
    camera = {};
  }
}

ResourceHandle SsrPass::AddToGraph(RenderGraph& graph, ResourceHandle scene_color,
                                   ResourceHandle depth, ResourceHandle normals,
                                   const Mat4& view_proj, const Mat4& inv_view_proj,
                                   const Vec3& camera_pos, u32 frame_index) {
  ResourceHandle out = graph.CreateTexture({.name = "ssr",
                                            .format = Format::kRGBA16Float,
                                            .width = extent_.width,
                                            .height = extent_.height});

  graph.AddPass(
      "ssr",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Read(scene_color, ResourceUsage::kSampledCompute);
        builder.Read(depth, ResourceUsage::kSampledCompute);
        builder.Read(normals, ResourceUsage::kSampledCompute);
        builder.Write(out, ResourceUsage::kStorageWrite);
      },
      [this, scene_color, depth, normals, out, view_proj, inv_view_proj, camera_pos, frame_index,
       slot = frame_index % 2](PassContext& ctx) {
        const SsrCamera camera{view_proj, inv_view_proj};
        std::memcpy(camera_[slot].mapped, &camera, sizeof(camera));

        SsrPush push{};
        push.camera_pos[0] = camera_pos.x;
        push.camera_pos[1] = camera_pos.y;
        push.camera_pos[2] = camera_pos.z;
        push.inv_size[0] = 1.0f / static_cast<f32>(extent_.width);
        push.inv_size[1] = 1.0f / static_cast<f32>(extent_.height);
        push.intensity = settings_.intensity;
        push.max_distance = settings_.max_distance;
        push.thickness = settings_.thickness;
        push.frame_index = static_cast<f32>(frame_index % 4096);
        push.step_count = settings_.step_count;

        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Storage(0, ctx.graph->image(out)),
                                   Bind::Sampled(1, ctx.graph->image(depth)),
                                   Bind::Sampled(2, ctx.graph->image(normals)),
                                   Bind::Sampled(3, ctx.graph->image(scene_color)),
                                   Bind::Uniform(4, camera_[slot], 0, sizeof(SsrCamera))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent_);
      });
  return out;
}

}  // namespace rx::render
