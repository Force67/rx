#include "rxe/render/geometry/fur.h"

#include <string.h>

#include "base/memory/mem_ops.h"
#include "foundation/logging/log.h"
#include "rxe/asset/primitives.h"
#include "rxe/gpu/rhi/device.h"
#include "shaders/fur_ps_hlsl.h"
#include "shaders/fur_vs_hlsl.h"

namespace rx::render {
namespace {

// The view projection alone is half the 128 bytes vulkan guarantees for a push
// block, and it is the same for every shell, so it rides in a uniform buffer
// and the push keeps the per-draw model and the scalars.
struct FurCamera {
  Mat4 view_proj;
};

struct FurPush {
  Mat4 model;
  f32 sun_dir[3];
  f32 fur_length;
  f32 sun_color[3];
  u32 shell_count;
  f32 base_color[3];
  f32 ambient;
};

}  // namespace

bool FurPass::Initialize(gpu::Device& device, gpu::Format color_format, gpu::Format depth_format) {
  asset::Mesh sphere = asset::MakeSphere(radius_, 64, 96, asset::MakeAssetId("builtin/fur/sphere"));
  const asset::MeshLod& lod = sphere.lods[0];
  index_count_ = static_cast<u32>(lod.indices.size());
  vertices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.vertices.data()),
               lod.vertices.size() * sizeof(asset::Vertex)),
      gpu::kBufferUsageVertex);
  indices_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(lod.indices.data()), lod.indices.size() * sizeof(u32)),
      gpu::kBufferUsageIndex);

  // TODO(rhi): blend preset mismatch: old alpha factors were ZERO/ONE (dst alpha
  // preserved); kAlpha uses ONE/ONE_MINUS_SRC_ALPHA.
  pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_fur_vs_hlsl),
      .fragment = RX_SHADER(k_fur_ps_hlsl),
      .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                          .attributes = {{0, gpu::Format::kRGB32Float,
                                          offsetof(asset::Vertex, position)},
                                         {1, gpu::Format::kRGB32Float, offsetof(asset::Vertex, normal)},
                                         {3, gpu::Format::kRG32Float, offsetof(asset::Vertex, uv)}}}},
      .raster = {.cull = gpu::CullMode::kBack, .front = gpu::FrontFace::kCounterClockwise},
      .depth = {.test = true,
                .write = false,  // shells alpha-blend; the core owns the depth
                .compare = gpu::CompareOp::kGreaterEqual,  // reversed z
                .format = depth_format},
      .color_formats = {color_format},
      .blend = {gpu::BlendMode::kAlpha},
      .sets = {{.slots = {{0, gpu::BindingType::kUniformBuffer}},
                .stages = gpu::kShaderStageVertex}},
      .push_constant_size = gpu::PushSize<FurPush>(),
      .debug_name = "fur",
  });
  if (!pipeline_) {
    RX_ERROR("fur pipeline creation failed");
    return false;
  }

  // One per in-flight frame: the pass rewrites it while the previous frame may
  // still be reading its own copy.
  for (gpu::GpuBuffer& camera : camera_) {
    camera = device.CreateBuffer(sizeof(FurCamera), gpu::kBufferUsageUniform, true);
    if (!camera.mapped) {
      RX_ERROR("fur camera uniform allocation failed");
      return false;
    }
  }
  return true;
}

void FurPass::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                         const Mat4& model, const Mat4& view_proj, const Vec3& sun_dir,
                         const Vec3& sun_color, f32 ambient, const Params& params) {
  camera_slot_ ^= 1u;
  const u32 slot = camera_slot_;
  graph.AddPass(
      "fur",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, slot, color, depth, model, view_proj, sun_dir, sun_color, ambient,
       params](PassContext& ctx) {
        const gpu::GpuImage& target = ctx.graph->image(color);
        gpu::ColorAttachment col[] = {{.view = target.view, .load = gpu::LoadOp::kLoad}};
        gpu::DepthAttachment dep{.view = ctx.graph->image(depth).view, .load = gpu::LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = target.extent, .colors = col, .depth = &dep});

        ctx.cmd->BindPipeline(pipeline_);
        const FurCamera camera{view_proj};
        base::MemCopy(camera_[slot].mapped, &camera, sizeof(camera));
        ctx.cmd->BindTransient(0,
                               {gpu::Bind::Uniform(0, camera_[slot], 0, sizeof(FurCamera))});

        FurPush push{};
        push.model = model;
        push.sun_dir[0] = sun_dir.x;
        push.sun_dir[1] = sun_dir.y;
        push.sun_dir[2] = sun_dir.z;
        push.fur_length = params.fur_length;
        push.sun_color[0] = sun_color.x;
        push.sun_color[1] = sun_color.y;
        push.sun_color[2] = sun_color.z;
        push.shell_count = params.shell_count;
        push.base_color[0] = params.base_color[0];
        push.base_color[1] = params.base_color[1];
        push.base_color[2] = params.base_color[2];
        push.ambient = ambient;
        ctx.cmd->Push(push);

        ctx.cmd->BindVertexBuffer(0, vertices_, 0);
        ctx.cmd->BindIndexBuffer(indices_, 0, gpu::IndexType::kUint32);
        ctx.cmd->DrawIndexed(index_count_, params.shell_count, 0, 0, 0);
        ctx.cmd->EndRendering();
      });
}

void FurPass::Destroy(gpu::Device& device) {
  device.DestroyPipeline(pipeline_);
  device.DestroyBuffer(vertices_);
  device.DestroyBuffer(indices_);
  for (gpu::GpuBuffer& camera : camera_) {
    if (camera) device.DestroyBuffer(camera);
    camera = {};
  }
  pipeline_ = {};
}

}  // namespace rx::render
