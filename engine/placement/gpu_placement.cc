#include "placement/gpu_placement.h"

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "placement/placement_pattern.h"
#include "render/rhi/command_list.h"
#include "shaders/placement_density_cs_hlsl.h"
#include "shaders/placement_generate_cs_hlsl.h"
#include "shaders/placement_transform_cs_hlsl.h"

namespace rx::placement {

using render::BarrierScope;
using render::BindingType;
using render::ResourceState;
namespace Bind = render::Bind;

namespace {

// GPU mirror of PlacementLayer (PlacementLayerGpu in placement_common.hlsli).
struct LayerGpu {
  u32 prog_offset;
  u32 prog_count;
  u32 flags;
  u32 pad;
  f32 scale_min;
  f32 scale_max;
  f32 tilt;
  f32 y_offset;
};

struct PointGpu {
  f32 position[3];
  f32 pad0;
  f32 normal[3];
  u32 layer;
  u32 point;
  i32 tile_x;
  i32 tile_z;
  u32 pad1;
};

struct InstanceGpu {
  Mat4 transform;
  u32 layer;
  u32 point;
  i32 tile_x;
  i32 tile_z;
};

struct DensityPush {
  f32 tile_origin[2];
  f32 tile_size;
  f32 pad0;
  f32 map_origin[2];
  f32 map_inv_extent;
  f32 pad1;
  u32 first_layer;
  u32 layer_count;
  u32 density_offset;
  u32 pad2;
};

struct GeneratePush {
  f32 tile_origin[2];
  f32 tile_size;
  f32 footprint;
  f32 map_origin[2];
  f32 map_inv_extent;
  f32 map_meters_per_texel;
  f32 jitter;
  u32 seed;
  u32 stack_index;
  u32 first_layer;
  u32 layer_count;
  u32 density_offset;
  u32 height_map;
  i32 tile_x;
  i32 tile_z;
  u32 point_capacity;
  u32 pad0;
  u32 pad1;
};

struct TransformPush {
  u32 seed;
  u32 point_capacity;
  u32 pad0;
  u32 pad1;
};

constexpr u32 kDensityFloatsPerJob =
    kDensityResolution * kDensityResolution * kMaxStackLayers;

}  // namespace

bool GpuPlacement::Initialize(render::Device& device, const PlacementSystem& system) {
  max_jobs_ = system.config().max_jobs_per_update;
  point_capacity_ = max_jobs_ * kPatternPointCount;

  density_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_placement_density_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(DensityPush),
      .debug_name = "placement_density",
  });
  generate_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_placement_generate_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(GeneratePush),
      .debug_name = "placement_generate",
  });
  transform_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_placement_transform_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(TransformPush),
      .debug_name = "placement_transform",
  });
  if (!density_pipeline_ || !generate_pipeline_ || !transform_pipeline_) {
    RX_ERROR("placement pipeline creation failed");
    return false;
  }

  // Static authored data: bytecode, layer descriptors, dither pattern.
  base::Vector<u32> ops;
  base::Vector<LayerGpu> layers;
  for (const PlacementLayer& layer : system.layers()) {
    LayerGpu gpu = {};
    gpu.prog_offset = static_cast<u32>(ops.size() / 4);
    gpu.prog_count = static_cast<u32>(layer.density.ops().size());
    gpu.flags = layer.random_yaw ? 1u : 0u;
    gpu.scale_min = layer.scale_min;
    gpu.scale_max = layer.scale_max;
    gpu.tilt = layer.tilt;
    gpu.y_offset = layer.y_offset;
    layers.push_back(gpu);
    for (const DensityOp& op : layer.density.ops()) {
      u32 raw[4];
      raw[0] = static_cast<u32>(op.op);
      std::memcpy(&raw[1], &op.a, sizeof(f32));
      std::memcpy(&raw[2], &op.b, sizeof(f32));
      std::memcpy(&raw[3], &op.c, sizeof(f32));
      for (u32 v : raw) ops.push_back(v);
    }
  }
  if (ops.empty()) ops.resize(4, 0u);  // dummy so the buffer exists

  ops_ = device.CreateBufferWithData(
      {reinterpret_cast<const u8*>(ops.data()), ops.size() * sizeof(u32)},
      render::kBufferUsageStorage);
  layers_ = device.CreateBufferWithData(
      {reinterpret_cast<const u8*>(layers.data()), layers.size() * sizeof(LayerGpu)},
      render::kBufferUsageStorage);
  pattern_ = device.CreateBufferWithData(
      {reinterpret_cast<const u8*>(kPatternXY), sizeof(kPatternXY)},
      render::kBufferUsageStorage);

  world_sampler_ = device.GetSampler({
      .min_filter = render::Filter::kLinear,
      .mag_filter = render::Filter::kLinear,
      .address_u = render::AddressMode::kClampToEdge,
      .address_v = render::AddressMode::kClampToEdge,
  });

  for (BufferSet& set : sets_) {
    set.density = device.CreateBuffer(
        static_cast<u64>(max_jobs_) * kDensityFloatsPerJob * sizeof(f32),
        render::kBufferUsageStorage);
    set.points = device.CreateBuffer(static_cast<u64>(point_capacity_) * sizeof(PointGpu),
                                     render::kBufferUsageStorage);
    set.counts = device.CreateBuffer(sizeof(u32), render::kBufferUsageStorage, true);
    set.instances =
        device.CreateBuffer(static_cast<u64>(point_capacity_) * sizeof(InstanceGpu),
                            render::kBufferUsageStorage, true);
    if (!set.counts.mapped || !set.instances.mapped) return false;
  }

  initialized_ = true;
  return true;
}

void GpuPlacement::Shutdown(render::Device& device) {
  if (!initialized_) return;
  device.WaitIdle();
  for (BufferSet& set : sets_) {
    device.DestroyBuffer(set.density);
    device.DestroyBuffer(set.points);
    device.DestroyBuffer(set.counts);
    device.DestroyBuffer(set.instances);
    set.jobs.clear();
  }
  device.DestroyBuffer(ops_);
  device.DestroyBuffer(layers_);
  device.DestroyBuffer(pattern_);
  device.DestroyImage(world_maps_);
  device.DestroyPipeline(density_pipeline_);
  device.DestroyPipeline(generate_pipeline_);
  device.DestroyPipeline(transform_pipeline_);
  initialized_ = false;
}

void GpuPlacement::SyncWorldData(render::Device& device, const WorldData& world) {
  const u32 resolution = world.resolution();
  const u32 map_count = world.map_count();
  bool recreate = !world_maps_ || synced_map_count_ != map_count;
  if (recreate) {
    device.WaitIdle();
    if (world_maps_) device.DestroyImage(world_maps_);
    world_maps_ = device.CreateImage2DArray(
        render::Format::kR32Float, resolution, resolution, map_count,
        render::kTextureUsageSampled | render::kTextureUsageTransferDst);
    synced_map_count_ = map_count;
    synced_revisions_.clear();
    synced_revisions_.resize(map_count, 0);
  }

  for (u32 map = 0; map < map_count; ++map) {
    if (synced_revisions_[map] == world.revision(map)) continue;
    std::span<const f32> texels = world.texels(map);
    render::GpuBuffer staging = device.CreateBuffer(
        texels.size() * sizeof(f32), render::kBufferUsageTransferSrc, true);
    std::memcpy(staging.mapped, texels.data(), texels.size() * sizeof(f32));
    const bool first_upload = synced_revisions_[map] == 0;
    device.ImmediateSubmit([&](render::CommandList& cmd) {
      cmd.Barrier(render::Transition(world_maps_,
                                     first_upload ? ResourceState::kUndefined
                                                  : ResourceState::kShaderReadCompute,
                                     ResourceState::kCopyDst));
      render::BufferTextureCopy copy;
      copy.array_layer = map;
      cmd.CopyBufferToTexture(staging, world_maps_, {&copy, 1});
      cmd.Barrier(render::Transition(world_maps_, ResourceState::kCopyDst,
                                     ResourceState::kShaderReadCompute));
    });
    device.DestroyBuffer(staging);
    synced_revisions_[map] = world.revision(map);
  }
}

void GpuPlacement::RecordBatch(render::CommandList& cmd, const PlacementSystem& system,
                               std::span<const TileKey> tiles, BufferSet& set) {
  const WorldData& world = system.world();
  const f32 map_inv_extent = 1.0f / world.extent();

  // Stage batching: every tile's DENSITYMAP first, one barrier, every
  // GENERATE, one barrier, then a single PLACEMENT sweep over all points and
  // one host-read barrier - instead of a dependency chain per tile.
  *reinterpret_cast<u32*>(set.counts.mapped) = 0;

  cmd.BindPipeline(density_pipeline_);
  cmd.BindTransient(0, {Bind::StorageBuffer(0, ops_), Bind::StorageBuffer(1, layers_),
                        Bind::StorageBuffer(2, set.density),
                        Bind::Combined(3, world_maps_.view, world_sampler_)});
  for (u32 job = 0; job < tiles.size(); ++job) {
    const TileKey& key = tiles[job];
    const PlacementStack& stack = system.stacks()[key.stack];
    DensityPush push = {};
    push.tile_origin[0] = static_cast<f32>(key.x) * stack.tile_size;
    push.tile_origin[1] = static_cast<f32>(key.z) * stack.tile_size;
    push.tile_size = stack.tile_size;
    push.map_origin[0] = world.origin_x();
    push.map_origin[1] = world.origin_z();
    push.map_inv_extent = map_inv_extent;
    push.first_layer = stack.first_layer;
    push.layer_count = stack.layer_count;
    push.density_offset = job * kDensityFloatsPerJob;
    cmd.Push(push);
    cmd.Dispatch(kDensityResolution / 8, kDensityResolution / 8, 1);
  }
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

  cmd.BindPipeline(generate_pipeline_);
  cmd.BindTransient(0, {Bind::StorageBuffer(0, pattern_), Bind::StorageBuffer(1, set.density),
                        Bind::StorageBuffer(2, set.points),
                        Bind::StorageBuffer(3, set.counts),
                        Bind::Combined(4, world_maps_.view, world_sampler_)});
  for (u32 job = 0; job < tiles.size(); ++job) {
    const TileKey& key = tiles[job];
    const PlacementStack& stack = system.stacks()[key.stack];
    GeneratePush push = {};
    push.tile_origin[0] = static_cast<f32>(key.x) * stack.tile_size;
    push.tile_origin[1] = static_cast<f32>(key.z) * stack.tile_size;
    push.tile_size = stack.tile_size;
    push.footprint = stack.footprint;
    push.map_origin[0] = world.origin_x();
    push.map_origin[1] = world.origin_z();
    push.map_inv_extent = map_inv_extent;
    push.map_meters_per_texel = world.meters_per_texel();
    push.jitter = system.config().jitter;
    push.seed = system.config().seed;
    push.stack_index = key.stack;
    push.first_layer = stack.first_layer;
    push.layer_count = stack.layer_count;
    push.density_offset = job * kDensityFloatsPerJob;
    push.height_map = system.height_map();
    push.tile_x = key.x;
    push.tile_z = key.z;
    push.point_capacity = point_capacity_;
    cmd.Push(push);
    cmd.Dispatch(1, 1, 1);
  }
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);

  cmd.BindPipeline(transform_pipeline_);
  cmd.BindTransient(0, {Bind::StorageBuffer(0, set.points), Bind::StorageBuffer(1, layers_),
                        Bind::StorageBuffer(2, set.instances),
                        Bind::StorageBuffer(3, set.counts)});
  TransformPush push = {};
  push.seed = system.config().seed;
  push.point_capacity = point_capacity_;
  cmd.Push(push);
  cmd.Dispatch((point_capacity_ + 63) / 64, 1, 1);
  cmd.MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kHostRead);
}

void GpuPlacement::RecordJobs(render::CommandList& cmd, PlacementSystem& system, u32 slot) {
  BufferSet& set = sets_[slot];
  set.jobs.clear();
  std::span<const TileKey> pending = system.pending();
  const u32 count = std::min<u32>(static_cast<u32>(pending.size()), max_jobs_);
  if (count == 0) return;
  for (u32 i = 0; i < count; ++i) {
    set.jobs.push_back(pending[i]);
    system.MarkInFlight(pending[i]);
  }
  RecordBatch(cmd, system, {set.jobs.data(), set.jobs.size()}, set);
}

void GpuPlacement::ReadResults(const BufferSet& set, base::Vector<PlacedInstance>& out) const {
  u32 count = *reinterpret_cast<const u32*>(set.counts.mapped);
  count = std::min(count, point_capacity_);
  const auto* records = reinterpret_cast<const InstanceGpu*>(set.instances.mapped);
  for (u32 i = 0; i < count; ++i) {
    PlacedInstance instance;
    instance.transform = records[i].transform;
    instance.layer = records[i].layer;
    instance.point = records[i].point;
    instance.tile_x = records[i].tile_x;
    instance.tile_z = records[i].tile_z;
    out.push_back(instance);
  }
}

void GpuPlacement::Consume(u32 slot, PlacementSystem& system,
                           base::Vector<PlacedInstance>& out,
                           base::Vector<TileKey>& out_tiles) {
  BufferSet& set = sets_[slot];
  if (set.jobs.empty()) return;
  ReadResults(set, out);
  for (const TileKey& key : set.jobs) {
    system.MarkLive(key);
    out_tiles.push_back(key);
  }
  set.jobs.clear();
}

void GpuPlacement::GenerateImmediate(render::Device& device, PlacementSystem& system,
                                     std::span<const TileKey> tiles,
                                     base::Vector<PlacedInstance>& out) {
  BufferSet& set = sets_[kBufferSets - 1];
  for (u32 offset = 0; offset < tiles.size(); offset += max_jobs_) {
    const u32 count = std::min<u32>(static_cast<u32>(tiles.size()) - offset, max_jobs_);
    std::span<const TileKey> batch = tiles.subspan(offset, count);
    device.ImmediateSubmit(
        [&](render::CommandList& cmd) { RecordBatch(cmd, system, batch, set); });
    ReadResults(set, out);
    for (const TileKey& key : batch) system.MarkLive(key);
  }
}

}  // namespace rx::placement
