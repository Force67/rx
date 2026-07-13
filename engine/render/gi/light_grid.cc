#include "render/gi/light_grid.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "shaders/light_grid_cs_hlsl.h"

namespace rx::render {
namespace {

struct LightGridPush {
  u32 light_count;
  u32 pad[3];
};

}  // namespace

bool LightGrid::Initialize(Device& device) {
  device_ = &device;
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_light_grid_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kUniformBuffer}}}},
      .push_constant_size = sizeof(LightGridPush),
      .debug_name = "light_grid",
  });
  if (!pipeline_) return false;

  for (GpuBuffer& b : params_buffers_) {
    b = device.CreateBuffer(sizeof(GridParams), kBufferUsageUniform, true);
    if (!b.mapped) return false;
  }
  counts_ = device.CreateBuffer(kTotalCells * sizeof(u32), kBufferUsageStorage);
  ids_ = device.CreateBuffer(static_cast<u64>(kTotalCells) * kMaxPerCell * sizeof(u32),
                             kBufferUsageStorage);
  if (!counts_ || !ids_) return false;
  return true;
}

void LightGrid::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (GpuBuffer& b : params_buffers_) device.DestroyBuffer(b);
  device.DestroyBuffer(counts_);
  device.DestroyBuffer(ids_);
}

void LightGrid::AddToGraph(RenderGraph& graph, const GpuBuffer& lights, u32 light_count,
                           const Vec3& camera, u32 frame_index, bool async) {
  // Snap each cascade around the camera to its own cell size (prevents crawling).
  GridParams params{};
  for (u32 c = 0; c < kCascades; ++c) {
    f32 extent = kCascade0Extent * static_cast<f32>(1u << c);
    f32 cell_size = extent / static_cast<f32>(kCells);
    Vec3 origin{std::floor((camera.x - extent * 0.5f) / cell_size) * cell_size,
                std::floor((camera.y - extent * 0.5f) / cell_size) * cell_size,
                std::floor((camera.z - extent * 0.5f) / cell_size) * cell_size};
    params.cascade[c][0] = origin.x;
    params.cascade[c][1] = origin.y;
    params.cascade[c][2] = origin.z;
    params.cascade[c][3] = cell_size;
  }
  params.info[0] = kCells;
  params.info[1] = kCascades;
  params.info[2] = kMaxPerCell;
  params.info[3] = 0;
  GpuBuffer& params_buffer = params_buffers_[frame_index % 2];
  std::memcpy(params_buffer.mapped, &params, sizeof(params));

  u32 capped = light_count < kMaxLights ? light_count : kMaxLights;
  graph.AddPass(
      "light_grid", [async](RenderGraph::PassBuilder& b) { if (async) b.Async(); },
      [this, &lights, capped, frame_index](PassContext& ctx) {
        const GpuBuffer& params_buffer = params_buffers_[frame_index % 2];
        LightGridPush push{capped, {0, 0, 0}};
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, lights, 0, lights.size),
                                   Bind::StorageBuffer(1, counts_, 0, counts_.size),
                                   Bind::StorageBuffer(2, ids_, 0, ids_.size),
                                   Bind::Uniform(3, params_buffer, 0, sizeof(GridParams))});
        ctx.cmd->Push(push);
        // 4x4x4 threads/group; groups cover 16^3 cells x kCascades in z.
        ctx.cmd->Dispatch(kCells / 4, kCells / 4, (kCells / 4) * kCascades);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
      });
}

}  // namespace rx::render
