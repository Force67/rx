#include "render/geometry/procedural_grass.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "shaders/procedural_grass_generate_cs_hlsl.h"
#include "shaders/procedural_grass_prepass_ps_hlsl.h"
#include "shaders/procedural_grass_ps_hlsl.h"
#include "shaders/procedural_grass_vs_hlsl.h"

namespace rx::render {
namespace {

struct alignas(16) GpuSurface {
  f32 p0_density[4];
  f32 p1_growth[4];
  f32 p2_type[4];
  u32 meta[4];  // first candidate, candidate count, stable surface id, unused
};
static_assert(sizeof(GpuSurface) == 64);

constexpr u32 kSampleCounts[4] = {1, 2, 4, 8};

bool AllFinite(const f32* values, u32 count) {
  for (u32 i = 0; i < count; ++i) {
    if (!std::isfinite(values[i]))
      return false;
  }
  return true;
}

}  // namespace

bool ProceduralGrass::Initialize(Device& device,
                                 Format scene_color,
                                 Format motion,
                                 Format normal,
                                 Format skin_diffuse,
                                 Format depth) {
  device_ = &device;
  scene_color_format_ = scene_color;
  motion_format_ = motion;
  normal_format_ = normal;
  skin_diffuse_format_ = skin_diffuse;
  depth_format_ = depth;
  generate_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_procedural_grass_generate_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kByteBuffer},
                          {1, BindingType::kByteBuffer},
                          {2, BindingType::kByteBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer},
                          {5, BindingType::kStorageBuffer}}}},
      .push_constant_size = sizeof(GenerationPush),
      .debug_name = "procedural_grass_generate",
  });
  if (!generate_pipeline_)
    return false;
  return CreateDrawPipelines(device, 1);
}

bool ProceduralGrass::CreateDrawPipelines(Device& device, u32 samples) {
  const u32 index = PipelineIndex(samples);
  if (prepass_pipelines_[index] && scene_pipelines_[index])
    return true;
  if ((failed_sample_mask_ & (1u << index)) != 0)
    return false;

  samples = kSampleCounts[index];
  const PipelineBindings draw_set{.slots = {{0, BindingType::kByteBuffer},
                                            {1, BindingType::kByteBuffer},
                                            {2, BindingType::kByteBuffer}}};

  PipelineHandle prepass = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_procedural_grass_vs_hlsl),
      .fragment = RX_SHADER(k_procedural_grass_prepass_ps_hlsl),
      .topology = PrimitiveTopology::kTriangleList,
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true,
                .write = true,
                .compare = CompareOp::kGreaterEqual,
                .format = depth_format_},
      .color_formats = {normal_format_, motion_format_, Format::kR32Float},
      .blend = {BlendMode::kOpaque, BlendMode::kOpaque, BlendMode::kOpaque},
      .sets = {draw_set},
      .push_constant_size = sizeof(DrawPush),
      .samples = samples,
      .debug_name = "procedural_grass_prepass",
  });
  PipelineHandle scene = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_procedural_grass_vs_hlsl),
      .fragment = RX_SHADER(k_procedural_grass_ps_hlsl),
      .topology = PrimitiveTopology::kTriangleList,
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true,
                .write = false,
                .compare = CompareOp::kEqual,
                .format = depth_format_},
      .color_formats = {scene_color_format_, motion_format_, skin_diffuse_format_},
      .blend = {BlendMode::kOpaque, BlendMode::kOpaque, BlendMode::kOpaque},
      .sets = {draw_set},
      .push_constant_size = sizeof(DrawPush),
      .samples = samples,
      .debug_name = "procedural_grass_scene",
  });
  if (!prepass || !scene) {
    if (prepass)
      device.DestroyPipeline(prepass);
    if (scene)
      device.DestroyPipeline(scene);
    failed_sample_mask_ |= 1u << index;
    return false;
  }
  prepass_pipelines_[index] = prepass;
  scene_pipelines_[index] = scene;
  return true;
}

bool ProceduralGrass::EnsureSampleCount(Device& device, u32 samples) {
  return available() && CreateDrawPipelines(device, samples);
}

bool ProceduralGrass::EnsureBuffers() {
  if (slots_[0].field)
    return true;
  if (!device_ || allocation_failed_)
    return false;

  constexpr u64 kFieldBytes = static_cast<u64>(kMaxFieldDimension) * kMaxFieldDimension *
                              sizeof(GrassFieldSample);
  constexpr u64 kTypeBytes = static_cast<u64>(kMaxTypes) * sizeof(GrassType);
  constexpr u64 kSurfaceBytes = static_cast<u64>(kMaxSurfaces) * sizeof(GpuSurface);
  constexpr u64 kInteractionBytes =
      static_cast<u64>(kMaxInteractions) * sizeof(GrassInteraction);
  constexpr u64 kInstanceBytes = static_cast<u64>(kMaxBlades) * 64u;
  for (Slot& slot : slots_) {
    slot.field = device_->CreateBuffer(kFieldBytes, kBufferUsageStorage, true);
    slot.types = device_->CreateBuffer(kTypeBytes, kBufferUsageStorage, true);
    slot.surfaces = device_->CreateBuffer(kSurfaceBytes, kBufferUsageStorage, true);
    slot.interactions =
        device_->CreateBuffer(kInteractionBytes, kBufferUsageStorage, true);
    slot.instances = device_->CreateBuffer(kInstanceBytes, kBufferUsageStorage);
    slot.args = device_->CreateBuffer(16, kBufferUsageStorage | kBufferUsageIndirect);
    slot.counters = device_->CreateBuffer(16, kBufferUsageStorage | kBufferUsageIndirect);
    if (!slot.field.mapped || !slot.types.mapped || !slot.surfaces.mapped ||
        !slot.interactions.mapped || !slot.instances || !slot.args || !slot.counters) {
      RX_ERROR("procedural grass buffer allocation failed");
      allocation_failed_ = true;
      for (Slot& allocated : slots_) {
        device_->DestroyBuffer(allocated.field);
        device_->DestroyBuffer(allocated.types);
        device_->DestroyBuffer(allocated.surfaces);
        device_->DestroyBuffer(allocated.interactions);
        device_->DestroyBuffer(allocated.instances);
        device_->DestroyBuffer(allocated.args);
        device_->DestroyBuffer(allocated.counters);
        allocated = {};
      }
      return false;
    }
  }
  return true;
}

void ProceduralGrass::Destroy(Device& device) {
  if (generate_pipeline_)
    device.DestroyPipeline(generate_pipeline_);
  generate_pipeline_ = {};
  for (u32 i = 0; i < 4; ++i) {
    if (prepass_pipelines_[i])
      device.DestroyPipeline(prepass_pipelines_[i]);
    if (scene_pipelines_[i])
      device.DestroyPipeline(scene_pipelines_[i]);
    prepass_pipelines_[i] = {};
    scene_pipelines_[i] = {};
  }
  for (Slot& slot : slots_) {
    device.DestroyBuffer(slot.field);
    device.DestroyBuffer(slot.types);
    device.DestroyBuffer(slot.surfaces);
    device.DestroyBuffer(slot.interactions);
    device.DestroyBuffer(slot.instances);
    device.DestroyBuffer(slot.args);
    device.DestroyBuffer(slot.counters);
    slot = {};
  }
  device_ = nullptr;
  allocation_failed_ = false;
  failed_sample_mask_ = 0;
  scene_color_format_ = Format::kUnknown;
  motion_format_ = Format::kUnknown;
  normal_format_ = Format::kUnknown;
  skin_diffuse_format_ = Format::kUnknown;
  depth_format_ = Format::kUnknown;
}

bool ProceduralGrass::Prepare(const GrassDomain& domain,
                              std::span<const GrassInteraction> interactions,
                              const Frame& frame,
                              u32 frame_slot) {
  if (!available() || frame_slot >= kFramesInFlight)
    return false;
  Slot& slot = slots_[frame_slot];
  slot.active = false;

  const u32 type_count = std::min(domain.type_count, kMaxTypes);
  const u32 surface_count =
      domain.surfaces ? std::min(domain.surface_count, kMaxSurfaces) : 0;
  const bool valid_field =
      domain.samples && domain.sample_width >= 2 && domain.sample_height >= 2 &&
      domain.sample_width <= kMaxFieldDimension &&
      domain.sample_height <= kMaxFieldDimension && std::isfinite(domain.origin_x) &&
      std::isfinite(domain.origin_z) && std::isfinite(domain.extent_x) &&
      std::isfinite(domain.extent_z) && domain.extent_x > 0.0f && domain.extent_z > 0.0f;
  if (type_count == 0 || !domain.types || (!valid_field && surface_count == 0))
    return false;
  if (!std::isfinite(frame.camera_pos.x) || !std::isfinite(frame.camera_pos.y) ||
      !std::isfinite(frame.camera_pos.z))
    return false;
  if (!AllFinite(frame.view_proj.m, 16) || !AllFinite(frame.prev_view_proj.m, 16))
    return false;
  const f32 frame_values[] = {
      frame.sun_direction.x, frame.sun_direction.y, frame.sun_direction.z,
      frame.sun_color.x,     frame.sun_color.y,     frame.sun_color.z,
      frame.sun_intensity,   frame.ambient,         frame.time,
      frame.delta_time,      frame.jitter[0],       frame.jitter[1],
      frame.wind_speed,      frame.wind_yaw,        frame.gustiness,
  };
  if (!AllFinite(frame_values,
                 static_cast<u32>(sizeof(frame_values) / sizeof(frame_values[0]))))
    return false;
  for (u32 i = 0; i < type_count; ++i) {
    const GrassType& type = domain.types[i];
    for (const f32* values :
         {type.base_color, type.tip_color, type.dimensions, type.shape, type.material}) {
      for (u32 value = 0; value < 4; ++value) {
        if (!std::isfinite(values[value]))
          return false;
      }
    }
  }
  if (!EnsureBuffers())
    return false;

  GrassGenerationSettings settings = SanitizeGrassSettings(domain.settings);
  const u32 max_surface_candidates = valid_field ? kMaxCandidates / 4u : kMaxCandidates;
  u32 reserved_surface_candidates = 0;
  for (u32 i = 0;
       i < surface_count && reserved_surface_candidates < max_surface_candidates; ++i) {
    const GrassSurfaceTriangle& surface = domain.surfaces[i];
    if (!std::isfinite(surface.density) || surface.density <= 0.0f ||
        !std::isfinite(surface.growth) || surface.growth <= 0.0f)
      continue;
    const u32 count = GrassSurfaceCandidateCount(surface, settings.candidate_spacing);
    reserved_surface_candidates +=
        std::min(count, max_surface_candidates - reserved_surface_candidates);
  }
  if (valid_field) {
    const u64 count = static_cast<u64>(domain.sample_width) * domain.sample_height;
    std::memcpy(slot.field.mapped, domain.samples, count * sizeof(GrassFieldSample));
  }
  std::memcpy(slot.types.mapped, domain.types, type_count * sizeof(GrassType));
  const u32 interaction_count =
      std::min(static_cast<u32>(interactions.size()), kMaxInteractions);
  if (interaction_count > 0) {
    std::memcpy(slot.interactions.mapped, interactions.data(),
                interaction_count * sizeof(GrassInteraction));
  }

  const f64 snapped_x =
      std::floor(static_cast<f64>(frame.camera_pos.x) / settings.stream_tile_size + 0.5) *
      settings.stream_tile_size;
  const f64 snapped_z =
      std::floor(static_cast<f64>(frame.camera_pos.z) / settings.stream_tile_size + 0.5) *
      settings.stream_tile_size;
  const f64 padded_radius = settings.stream_radius + settings.stream_tile_size * 0.5;
  u32 cells_x = valid_field ? static_cast<u32>(std::ceil(padded_radius * 2.0 /
                                                         settings.candidate_spacing)) +
                                  1u
                            : 0u;
  u32 cells_z = cells_x;
  u64 terrain_candidates = static_cast<u64>(cells_x) * cells_z;
  const u32 terrain_budget = kMaxCandidates - reserved_surface_candidates;
  bool grid_capped = false;
  if (terrain_candidates > terrain_budget) {
    const u32 side = static_cast<u32>(std::sqrt(static_cast<f32>(terrain_budget)));
    cells_x = side;
    cells_z = side;
    terrain_candidates = static_cast<u64>(side) * side;
    grid_capped = true;
  }
  const f64 grid_center_x = grid_capped ? frame.camera_pos.x : snapped_x;
  const f64 grid_center_z = grid_capped ? frame.camera_pos.z : snapped_z;
  const f64 center_cell_x = std::floor(grid_center_x / settings.candidate_spacing);
  const f64 center_cell_z = std::floor(grid_center_z / settings.candidate_spacing);
  const f64 min_cell_x_value = center_cell_x - static_cast<f64>(cells_x / 2u);
  const f64 min_cell_z_value = center_cell_z - static_cast<f64>(cells_z / 2u);
  if (min_cell_x_value < std::numeric_limits<i32>::min() ||
      min_cell_x_value > std::numeric_limits<i32>::max() ||
      min_cell_z_value < std::numeric_limits<i32>::min() ||
      min_cell_z_value > std::numeric_limits<i32>::max())
    return false;
  const i32 min_cell_x = static_cast<i32>(min_cell_x_value);
  const i32 min_cell_z = static_cast<i32>(min_cell_z_value);
  f32 active_radius = settings.stream_radius;
  if (grid_capped) {
    active_radius = std::min(active_radius, (static_cast<f32>(cells_x) * 0.5f - 1.0f) *
                                                settings.candidate_spacing);
  }
  const f32 fade_start = std::min(
      settings.fade_start, std::max(active_radius - settings.candidate_spacing, 0.0f));
  const f32 fade_end = std::clamp(settings.fade_end,
                                  fade_start + settings.candidate_spacing, active_radius);

  auto* gpu_surfaces = static_cast<GpuSurface*>(slot.surfaces.mapped);
  u32 surface_candidates = 0;
  u32 copied_surfaces = 0;
  for (u32 i = 0; i < surface_count; ++i) {
    const GrassSurfaceTriangle& src = domain.surfaces[i];
    if (!std::isfinite(src.density) || src.density <= 0.0f ||
        !std::isfinite(src.growth) || src.growth <= 0.0f)
      continue;
    u32 count = GrassSurfaceCandidateCount(src, settings.candidate_spacing);
    const u32 remaining =
        kMaxCandidates -
        std::min<u32>(kMaxCandidates,
                      static_cast<u32>(terrain_candidates) + surface_candidates);
    count = std::min(count, remaining);
    if (count == 0)
      continue;
    GpuSurface& dst = gpu_surfaces[copied_surfaces++];
    std::copy_n(src.p0, 3, dst.p0_density);
    dst.p0_density[3] =
        std::isfinite(src.density) ? std::clamp(src.density, 0.0f, 1.0f) : 0.0f;
    std::copy_n(src.p1, 3, dst.p1_growth);
    dst.p1_growth[3] = std::isfinite(src.growth) ? std::max(src.growth, 0.0f) : 0.0f;
    std::copy_n(src.p2, 3, dst.p2_type);
    dst.p2_type[3] = std::bit_cast<f32>(std::min(src.type, type_count - 1));
    dst.meta[0] = surface_candidates;
    dst.meta[1] = count;
    dst.meta[2] = src.surface_id;
    dst.meta[3] = 0;
    surface_candidates += count;
    if (static_cast<u64>(terrain_candidates) + surface_candidates >= kMaxCandidates)
      break;
  }

  if (valid_field) {
    const u64 field_bytes = static_cast<u64>(domain.sample_width) * domain.sample_height *
                            sizeof(GrassFieldSample);
    device_->FlushBuffer(slot.field, 0, field_bytes);
  }
  device_->FlushBuffer(slot.types, 0, static_cast<u64>(type_count) * sizeof(GrassType));
  if (interaction_count > 0) {
    device_->FlushBuffer(slot.interactions, 0,
                         static_cast<u64>(interaction_count) * sizeof(GrassInteraction));
  }
  if (copied_surfaces > 0) {
    device_->FlushBuffer(slot.surfaces, 0,
                         static_cast<u64>(copied_surfaces) * sizeof(GpuSurface));
  }

  const u32 terrain_count = static_cast<u32>(terrain_candidates);
  const u32 candidate_count = terrain_count + surface_candidates;
  if (candidate_count == 0)
    return false;

  GenerationPush generation{};
  generation.view_proj = frame.view_proj;
  generation.field_origin_extent[0] = domain.origin_x;
  generation.field_origin_extent[1] = domain.origin_z;
  generation.field_origin_extent[2] = domain.extent_x;
  generation.field_origin_extent[3] = domain.extent_z;
  generation.field[0] = valid_field ? domain.sample_width : 0;
  generation.field[1] = valid_field ? domain.sample_height : 0;
  generation.field[2] = type_count;
  generation.field[3] = domain.seed;
  generation.camera_stream[0] = frame.camera_pos.x;
  generation.camera_stream[1] = frame.camera_pos.y;
  generation.camera_stream[2] = frame.camera_pos.z;
  generation.camera_stream[3] = active_radius;
  generation.placement[0] = settings.candidate_spacing;
  generation.placement[1] = settings.stream_tile_size;
  generation.grid[0] = min_cell_x;
  generation.grid[1] = min_cell_z;
  generation.grid[2] = static_cast<i32>(cells_x);
  generation.grid[3] = static_cast<i32>(cells_z);
  generation.counts[0] = terrain_count;
  generation.counts[1] = candidate_count;
  generation.counts[2] = copied_surfaces;
  generation.counts[3] = std::min(settings.max_blades, kMaxBlades);
  generation.density_lod[0] = settings.density_lod_start;
  generation.density_lod[1] = settings.density_lod_end;
  generation.density_lod[2] = settings.far_density;
  generation.density_lod[3] = settings.max_slope_cos;
  generation.geometry_fade[0] = settings.geometry_lod_start;
  generation.geometry_fade[1] = settings.geometry_lod_end;
  generation.geometry_fade[2] = fade_start;
  generation.geometry_fade[3] = fade_end;
  slot.generation = generation;

  DrawPush draw{};
  draw.view_proj = frame.view_proj;
  draw.prev_view_proj = frame.prev_view_proj;
  draw.camera_time[0] = frame.camera_pos.x;
  draw.camera_time[1] = frame.camera_pos.y;
  draw.camera_time[2] = frame.camera_pos.z;
  draw.camera_time[3] = frame.time;
  draw.sun_direction_intensity[0] = frame.sun_direction.x;
  draw.sun_direction_intensity[1] = frame.sun_direction.y;
  draw.sun_direction_intensity[2] = frame.sun_direction.z;
  draw.sun_direction_intensity[3] = frame.sun_intensity;
  draw.sun_color_ambient[0] = frame.sun_color.x;
  draw.sun_color_ambient[1] = frame.sun_color.y;
  draw.sun_color_ambient[2] = frame.sun_color.z;
  draw.sun_color_ambient[3] = frame.ambient;
  draw.wind[0] = frame.wind_speed;
  draw.wind[1] = frame.wind_yaw;
  draw.wind[2] = frame.gustiness;
  draw.wind[3] = std::clamp(frame.delta_time, 0.0f, 0.1f);
  draw.jitter_lod[0] = frame.jitter[0];
  draw.jitter_lod[1] = frame.jitter[1];
  draw.jitter_lod[2] = settings.geometry_lod_start;
  draw.jitter_lod[3] = settings.geometry_lod_end;
  draw.control[0] = interaction_count;
  draw.control[1] = type_count;
  draw.control[2] = kVerticesPerBlade;
  slot.draw = draw;
  slot.active = true;
  return true;
}

void ProceduralGrass::AddGeneration(RenderGraph& graph, u32 frame_slot) {
  if (frame_slot >= kFramesInFlight || !slots_[frame_slot].active)
    return;
  Slot& slot = slots_[frame_slot];
  const GenerationPush push = slot.generation;
  graph.AddPass(
      "procedural_grass_generate", [](RenderGraph::PassBuilder&) {},
      [this, frame_slot, push](PassContext& ctx) {
        Slot& current = slots_[frame_slot];
        ctx.cmd->BindPipeline(generate_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::ByteBuffer(0, current.field), Bind::ByteBuffer(1, current.types),
                Bind::ByteBuffer(2, current.surfaces),
                Bind::StorageBuffer(3, current.instances),
                Bind::StorageBuffer(4, current.args),
                Bind::StorageBuffer(5, current.counters)});

        GenerationPush phase = push;
        phase.control[0] = 0;  // reset append counter and indirect command
        ctx.cmd->Push(phase);
        ctx.cmd->Dispatch(1, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite,
                               BarrierScope::kComputeReadWrite);

        phase.control[0] = 1;  // cull and append visible blades
        ctx.cmd->Push(phase);
        ctx.cmd->Dispatch((phase.counts[1] + 63) / 64, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite,
                               BarrierScope::kComputeReadWrite);

        phase.control[0] = 2;  // publish the clamped indirect draw
        ctx.cmd->Push(phase);
        ctx.cmd->Dispatch(1, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kIndirectArgs);
      });
}

u32 ProceduralGrass::PipelineIndex(u32 samples) {
  if (samples >= 8)
    return 3;
  if (samples >= 4)
    return 2;
  if (samples >= 2)
    return 1;
  return 0;
}

void ProceduralGrass::Draw(CommandList& cmd,
                           u32 frame_slot,
                           PipelineHandle pipeline) const {
  if (frame_slot >= kFramesInFlight || !slots_[frame_slot].active || !pipeline)
    return;
  const Slot& slot = slots_[frame_slot];
  cmd.BindPipeline(pipeline);
  cmd.BindTransient(
      0, {Bind::ByteBuffer(0, slot.instances), Bind::ByteBuffer(1, slot.interactions),
          Bind::ByteBuffer(2, slot.types)});
  cmd.Push(slot.draw);
  cmd.DrawIndirectCount(slot.args, 0, slot.counters, 0, 1, 16);
}

void ProceduralGrass::DrawPrepass(CommandList& cmd, u32 frame_slot, u32 samples) const {
  Draw(cmd, frame_slot, prepass_pipelines_[PipelineIndex(samples)]);
}

void ProceduralGrass::DrawScene(CommandList& cmd, u32 frame_slot, u32 samples) const {
  Draw(cmd, frame_slot, scene_pipelines_[PipelineIndex(samples)]);
}

}  // namespace rx::render
