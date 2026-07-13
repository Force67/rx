#include "render/geometry/adaptive_water.h"

#include <algorithm>
#include <vector>

#include "asset/mesh.h"
#include "core/log.h"
#include "shaders/adaptive_water_cs_hlsl.h"

namespace rx::render {
namespace {

struct AdaptivePush {
  Mat4 local_to_clip;
  f32 bounds[4];
  f32 camera_height_time[4];  // local camera xz, surface height, time
  f32 metrics[4];             // target px, render width/height, unused
  u32 control[4];             // phase, nodes/tree, max depth, triangle budget
};
static_assert(sizeof(AdaptivePush) == 128);

}  // namespace

bool AdaptiveWaterMesh::Initialize(Device& device) {
  pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_adaptive_water_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(AdaptivePush),
      .debug_name = "adaptive_water_cbt",
  });
  if (!pipeline_) return false;

  std::vector<u32> zero_states(kNodeCount, 0);
  std::vector<DrawCommand> zero_commands(kMaxTriangles, DrawCommand{});
  const u32 counters[4] = {2, 0, 0, 0};
  states_ = device.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(zero_states.data()), zero_states.size() * sizeof(u32)),
      kBufferUsageStorage);
  counters_ =
      device.CreateBufferWithData(ByteSpan(reinterpret_cast<const u8*>(counters), sizeof(counters)),
                                  kBufferUsageStorage | kBufferUsageIndirect);
  vertices_ = device.CreateBuffer(static_cast<u64>(kNodeCount) * 3 * sizeof(asset::Vertex),
                                  kBufferUsageStorage | kBufferUsageVertex);
  commands_ =
      device.CreateBufferWithData(ByteSpan(reinterpret_cast<const u8*>(zero_commands.data()),
                                           zero_commands.size() * sizeof(DrawCommand)),
                                  kBufferUsageStorage | kBufferUsageIndirect);
  if (!states_ || !counters_ || !vertices_ || !commands_) {
    RX_WARN("adaptive water allocation failed; using authored geometry");
    Destroy(device);
    return false;
  }
  return true;
}

void AdaptiveWaterMesh::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  device.DestroyBuffer(states_);
  device.DestroyBuffer(counters_);
  device.DestroyBuffer(vertices_);
  device.DestroyBuffer(commands_);
}

void AdaptiveWaterMesh::Update(CommandList& cmd, const UpdateParams& params) {
  if (!available()) return;
  AdaptivePush push{};
  push.local_to_clip = params.local_to_clip;
  std::copy_n(params.bounds, 4, push.bounds);
  push.camera_height_time[0] = params.camera_local.x;
  push.camera_height_time[1] = params.camera_local.z;
  push.camera_height_time[2] = params.height;
  push.camera_height_time[3] = params.time;
  push.metrics[0] = std::max(params.target_pixels, 1.0f);
  push.metrics[1] = static_cast<f32>(std::max(params.render_width, 1u));
  push.metrics[2] = static_cast<f32>(std::max(params.render_height, 1u));
  push.control[1] = kNodesPerTree;
  push.control[2] = kMaxDepth;
  push.control[3] = SanitizeBudget(params.triangle_budget);

  cmd.BindPipeline(pipeline_);
  cmd.BindTransient(0, {Bind::StorageBuffer(0, states_), Bind::StorageBuffer(1, counters_),
                        Bind::StorageBuffer(2, vertices_), Bind::StorageBuffer(3, commands_)});
  const u32 groups = (kNodeCount + 63) / 64;
  if (surface_key_ != params.surface_key) {
    push.control[0] = 0;  // reset the two roots and invalidate every old command
    cmd.Push(push);
    cmd.Dispatch(groups, 1, 1);
    cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
    surface_key_ = params.surface_key;
  }

  push.control[0] = 1;  // merge complete sibling pairs
  cmd.Push(push);
  cmd.Dispatch(groups, 1, 1);
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

  push.control[0] = 2;  // split leaves (separate dispatch avoids merge races)
  cmd.Push(push);
  cmd.Dispatch(groups, 1, 1);
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

  push.control[0] = 3;  // materialize dirty leaf vertices
  cmd.Push(push);
  cmd.Dispatch(groups, 1, 1);
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);

  push.control[0] = 4;  // reset compact draw count
  cmd.Push(push);
  cmd.Dispatch(1, 1, 1);
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

  push.control[0] = 5;  // compact active leaves into a budget-sized draw stream
  cmd.Push(push);
  cmd.Dispatch(groups, 1, 1);
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

  push.control[0] = 6;  // clamp the counted draw to a newly lowered budget
  cmd.Push(push);
  cmd.Dispatch(1, 1, 1);
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kIndirectArgs);
}

void AdaptiveWaterMesh::Draw(CommandList& cmd) const {
  if (!available()) return;
  cmd.BindVertexBuffer(0, vertices_);
  cmd.DrawIndirectCount(commands_, 0, counters_, kIndirectCountOffset, kMaxTriangles,
                        sizeof(DrawCommand));
}

}  // namespace rx::render
