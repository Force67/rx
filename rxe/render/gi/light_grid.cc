#include "rxe/render/gi/light_grid.h"

#include <math.h>
#include <string.h>

#include "base/memory/mem_ops.h"
#include "foundation/logging/log.h"
#include "shaders/light_grid_cs_hlsl.h"

namespace rx::render {
namespace {

struct LightGridPush {
  u32 light_count;
  u32 pad[3];
};

}  // namespace

bool LightGrid::Initialize(gpu::Device& device) {
  device_ = &device;
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_light_grid_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageBuffer},
                          {1, gpu::BindingType::kStorageBuffer},
                          {2, gpu::BindingType::kStorageBuffer},
                          {3, gpu::BindingType::kUniformBuffer}}}},
      .push_constant_size = gpu::PushSize<LightGridPush>(),
      .debug_name = "light_grid",
  });
  if (!pipeline_) return false;

  for (gpu::GpuBuffer& b : params_buffers_) {
    b = device.CreateBuffer(sizeof(GridParams), gpu::kBufferUsageUniform, true);
    if (!b.mapped) return false;
  }
  counts_ = device.CreateBuffer(kTotalCells * sizeof(u32), gpu::kBufferUsageStorage);
  ids_ = device.CreateBuffer(static_cast<u64>(kTotalCells) * kMaxPerCell * sizeof(u32),
                             gpu::kBufferUsageStorage);
  if (!counts_ || !ids_) return false;
  return true;
}

void LightGrid::Destroy(gpu::Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (gpu::GpuBuffer& b : params_buffers_) device.DestroyBuffer(b);
  device.DestroyBuffer(counts_);
  device.DestroyBuffer(ids_);
}

void LightGrid::AddToGraph(RenderGraph& graph, const gpu::GpuBuffer& lights, u32 light_count,
                           const Vec3& camera, u32 frame_index, bool async) {
  // Snap each cascade around the camera to its own cell size (prevents crawling).
  GridParams params{};
  for (u32 c = 0; c < kCascades; ++c) {
    f32 extent = kCascade0Extent * static_cast<f32>(1u << c);
    f32 cell_size = extent / static_cast<f32>(kCells);
    Vec3 origin{::floorf((camera.x - extent * 0.5f) / cell_size) * cell_size,
                ::floorf((camera.y - extent * 0.5f) / cell_size) * cell_size,
                ::floorf((camera.z - extent * 0.5f) / cell_size) * cell_size};
    params.cascade[c][0] = origin.x;
    params.cascade[c][1] = origin.y;
    params.cascade[c][2] = origin.z;
    params.cascade[c][3] = cell_size;
  }
  params.info[0] = kCells;
  params.info[1] = kCascades;
  params.info[2] = kMaxPerCell;
  params.info[3] = 0;
  gpu::GpuBuffer& params_buffer = params_buffers_[frame_index % 2];
  base::MemCopy(params_buffer.mapped, &params, sizeof(params));

  u32 capped = light_count < kMaxLights ? light_count : kMaxLights;
  graph.AddPass(
      "light_grid", [async](RenderGraph::PassBuilder& b) { if (async) b.Async(); },
      [this, &lights, capped, frame_index](PassContext& ctx) {
        const gpu::GpuBuffer& params_buffer = params_buffers_[frame_index % 2];
        LightGridPush push{capped, {0, 0, 0}};
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {gpu::Bind::StorageBuffer(0, lights, 0, lights.size),
                                   gpu::Bind::StorageBuffer(1, counts_, 0, counts_.size),
                                   gpu::Bind::StorageBuffer(2, ids_, 0, ids_.size),
                                   gpu::Bind::Uniform(3, params_buffer, 0, sizeof(GridParams))});
        ctx.cmd->Push(push);
        // 4x4x4 threads/group; groups cover 16^3 cells x kCascades in z.
        ctx.cmd->Dispatch(kCells / 4, kCells / 4, (kCells / 4) * kCascades);
        ctx.cmd->MemoryBarrier(gpu::BarrierScope::kComputeWrite, gpu::BarrierScope::kComputeRead);
      });
}

}  // namespace rx::render
