#include "render/geometry/procedural_grass.h"

#include <bit>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "shaders/procedural_grass_bend_cs_hlsl.h"
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

struct CandidateRing {
  i32 min_fine_x = 0;
  i32 min_fine_z = 0;
  u32 cells = 0;
  u32 stride = 1;
  u32 candidates = 0;
  f32 inner_radius = 0.0f;
  f32 outer_radius = 0.0f;
  f32 refinement_start = 0.0f;
  u32 next_stride = 0;
};

bool BuildCandidateRing(const Vec3& camera,
                        f64 snapped_x,
                        f64 snapped_z,
                        f32 tile_size,
                        f32 base_spacing,
                        f32 inner_radius,
                        f32 outer_radius,
                        u32 stride,
                        u32 budget,
                        CandidateRing* out) {
  if (!out || outer_radius <= inner_radius || budget == 0)
    return false;
  const f64 spacing = static_cast<f64>(base_spacing) * stride;
  const f64 padded_radius = outer_radius + tile_size * 0.5;
  const u64 requested_side =
      static_cast<u64>(std::ceil(padded_radius * 2.0 / spacing)) + 1u;
  const u64 requested = requested_side * requested_side;
  const bool capped = requested > budget;
  const u32 cells = capped
                        ? static_cast<u32>(std::floor(std::sqrt(static_cast<f64>(budget))))
                        : static_cast<u32>(requested_side);
  if (cells == 0)
    return false;

  const f64 center_x = capped ? camera.x : snapped_x;
  const f64 center_z = capped ? camera.z : snapped_z;
  const f64 center_cell_x = std::floor(center_x / spacing);
  const f64 center_cell_z = std::floor(center_z / spacing);
  const f64 min_fine_x =
      (center_cell_x - static_cast<f64>(cells / 2u)) * stride;
  const f64 min_fine_z =
      (center_cell_z - static_cast<f64>(cells / 2u)) * stride;
  const f64 max_fine_x = min_fine_x + static_cast<f64>(cells - 1u) * stride;
  const f64 max_fine_z = min_fine_z + static_cast<f64>(cells - 1u) * stride;
  if (min_fine_x < std::numeric_limits<i32>::min() ||
      min_fine_x > std::numeric_limits<i32>::max() ||
      min_fine_z < std::numeric_limits<i32>::min() ||
      min_fine_z > std::numeric_limits<i32>::max() ||
      max_fine_x > std::numeric_limits<i32>::max() ||
      max_fine_z > std::numeric_limits<i32>::max())
    return false;

  f32 actual_outer = outer_radius;
  if (capped) {
    const f32 covered_radius =
        std::max((static_cast<f32>(cells) * 0.5f - 1.0f) *
                     static_cast<f32>(spacing),
                 0.0f);
    actual_outer = std::min(actual_outer, covered_radius);
  }
  if (actual_outer <= inner_radius)
    return false;

  out->min_fine_x = static_cast<i32>(min_fine_x);
  out->min_fine_z = static_cast<i32>(min_fine_z);
  out->cells = cells;
  out->stride = stride;
  out->candidates = cells * cells;
  out->inner_radius = inner_radius;
  out->outer_radius = actual_outer;
  return true;
}

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
  bend_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_procedural_grass_bend_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kCombinedTextureSampler},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kByteBuffer},
                          {3, BindingType::kCombinedTextureSampler},
                          {4, BindingType::kStorageImage},
                          {5, BindingType::kCombinedTextureSampler},
                          {6, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(BendPush),
      .debug_name = "procedural_grass_bend",
  });
  bend_sampler_ = device.GetSampler({.min_filter = Filter::kNearest,
                                     .mag_filter = Filter::kNearest,
                                     .address_u = AddressMode::kClampToEdge,
                                     .address_v = AddressMode::kClampToEdge});
  generate_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_procedural_grass_generate_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kByteBuffer},
                          {1, BindingType::kByteBuffer},
                          {2, BindingType::kByteBuffer},
                           {3, BindingType::kStorageBuffer},
                           {4, BindingType::kStorageBuffer},
                           {5, BindingType::kStorageBuffer},
                           {6, BindingType::kCombinedTextureSampler},
                           {7, BindingType::kCombinedTextureSampler},
                           {8, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(GenerationPush),
      .debug_name = "procedural_grass_generate",
  });
  if (!bend_pipeline_ || !bend_sampler_ || !generate_pipeline_) {
    Destroy(device);
    return false;
  }
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
  constexpr u64 kInstanceBytes = static_cast<u64>(kMaxBlades) * kInstanceStride;
  for (Slot& slot : slots_) {
    slot.field = device_->CreateBuffer(kFieldBytes, kBufferUsageStorage, true);
    slot.types = device_->CreateBuffer(kTypeBytes, kBufferUsageStorage, true);
    slot.surfaces = device_->CreateBuffer(kSurfaceBytes, kBufferUsageStorage, true);
    slot.interactions =
        device_->CreateBuffer(kInteractionBytes, kBufferUsageStorage, true);
    slot.instances = device_->CreateBuffer(kInstanceBytes, kBufferUsageStorage);
    slot.args = device_->CreateBuffer(32, kBufferUsageStorage | kBufferUsageIndirect);
    slot.counters = device_->CreateBuffer(32, kBufferUsageStorage | kBufferUsageIndirect);
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
  const TextureUsageFlags bend_usage =
      kTextureUsageSampled | kTextureUsageStorage | kTextureUsageTransferDst;
  for (u32 i = 0; i < 2; ++i) {
    bend_fields_[i] = device_->CreateImage2D(
        Format::kRGBA16Float, {kBendResolution, kBendResolution}, bend_usage);
    bend_metadata_[i] = device_->CreateImage2D(
        Format::kRGBA16Float, {kBendResolution, kBendResolution}, bend_usage);
    bend_confidence_[i] = device_->CreateImage2D(
        Format::kRG16Float, {kBendResolution, kBendResolution}, bend_usage);
    if (!bend_fields_[i] || !bend_metadata_[i] || !bend_confidence_[i]) {
      RX_ERROR("procedural grass bend field allocation failed");
      allocation_failed_ = true;
      for (GpuImage& allocated : bend_fields_) {
        device_->DestroyImage(allocated);
        allocated = {};
      }
      for (GpuImage& allocated : bend_metadata_) {
        device_->DestroyImage(allocated);
        allocated = {};
      }
      for (GpuImage& allocated : bend_confidence_) {
        device_->DestroyImage(allocated);
        allocated = {};
      }
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
  device_->ImmediateSubmit([this](CommandList& cmd) {
    const f32 zero[4] = {};
    for (u32 i = 0; i < 2; ++i) {
      for (GpuImage* image : {&bend_fields_[i], &bend_metadata_[i],
                              &bend_confidence_[i]}) {
        cmd.Barrier(Transition(*image, ResourceState::kUndefined,
                               ResourceState::kCopyDst));
        cmd.ClearColor(*image, zero);
        cmd.Barrier(Transition(*image, ResourceState::kCopyDst,
                               ResourceState::kShaderReadCompute));
      }
    }
  });
  return true;
}

void ProceduralGrass::Destroy(Device& device) {
  if (bend_pipeline_)
    device.DestroyPipeline(bend_pipeline_);
  bend_pipeline_ = {};
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
  for (GpuImage& field : bend_fields_) {
    device.DestroyImage(field);
    field = {};
  }
  for (GpuImage& field : bend_metadata_) {
    device.DestroyImage(field);
    field = {};
  }
  for (GpuImage& field : bend_confidence_) {
    device.DestroyImage(field);
    field = {};
  }
  bend_sampler_ = {};
  bend_origin_[0] = 0.0f;
  bend_origin_[1] = 0.0f;
  bend_extent_ = 1.0f;
  bend_height_origin_ = 0.0f;
  bend_last_update_time_ = 0.0f;
  bend_max_strength_ = 0.0f;
  bend_write_index_ = 0;
  bend_domain_seed_ = 0;
  bend_sample_source_ = nullptr;
  bend_surface_source_ = nullptr;
  bend_history_valid_ = false;
  bend_history_active_ = false;
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
  const u32 spacing_bits = std::bit_cast<u32>(settings.candidate_spacing);
  SurfaceUploadState& surface_upload = slot.surface_upload;
  const bool cache_surface_source = domain.surface_revision != 0 && domain.surfaces;
  const bool surface_source_hit =
      cache_surface_source && surface_upload.source_valid &&
      surface_upload.source == domain.surfaces &&
      surface_upload.revision == domain.surface_revision &&
      surface_upload.source_count == surface_count &&
      surface_upload.spacing_bits == spacing_bits &&
      surface_upload.reserve_limit == max_surface_candidates;
  if (!surface_source_hit) {
    surface_upload.packed_valid = false;
    if (cache_surface_source) {
      surface_upload.source = domain.surfaces;
      surface_upload.revision = domain.surface_revision;
      surface_upload.source_count = surface_count;
      surface_upload.spacing_bits = spacing_bits;
      surface_upload.reserve_limit = max_surface_candidates;
      surface_upload.source_valid = true;
    } else {
      surface_upload.source_valid = false;
    }
  }

  const f64 snapped_x =
      std::floor(static_cast<f64>(frame.camera_pos.x) / settings.stream_tile_size + 0.5) *
      settings.stream_tile_size;
  const f64 snapped_z =
      std::floor(static_cast<f64>(frame.camera_pos.z) / settings.stream_tile_size + 0.5) *
      settings.stream_tile_size;
  std::array<CandidateRing, 3> rings{};
  u32 ring_count = 0;
  u32 terrain_candidates = 0;
  f32 terrain_radius = 0.0f;
  if (valid_field) {
    const f32 lod_start = std::min(settings.density_lod_start, settings.stream_radius);
    const f32 lod_end = std::min(settings.density_lod_end, settings.stream_radius);
    const f32 lod_mid = (lod_start + lod_end) * 0.5f;
    const f32 desired_inner[3] = {0.0f, lod_start, lod_mid};
    const f32 ring_outer[3] = {lod_mid, lod_end, settings.stream_radius};
    const u32 ring_stride[3] = {1u, 2u, 4u};
    f32 covered_radius = 0.0f;
    for (u32 i = 0; i < 3; ++i) {
      CandidateRing ring;
      const f32 inner_radius = std::min(desired_inner[i], covered_radius);
      if (!BuildCandidateRing(frame.camera_pos, snapped_x, snapped_z,
                               settings.stream_tile_size,
                               settings.candidate_spacing, inner_radius,
                               ring_outer[i], ring_stride[i], kMaxCandidates,
                               &ring))
        continue;
      rings[ring_count++] = ring;
      terrain_candidates += ring.candidates;
      covered_radius = ring.outer_radius;
      terrain_radius = covered_radius;
    }
    for (u32 i = 0; i + 1u < ring_count; ++i) {
      rings[i].refinement_start = rings[i + 1u].inner_radius;
      rings[i].next_stride = rings[i + 1u].stride;
    }
  }

  FieldUploadState& field_upload = slot.field_upload;
  const bool field_hit =
      valid_field && domain.sample_revision != 0 && field_upload.valid &&
      field_upload.source == domain.samples &&
      field_upload.revision == domain.sample_revision &&
      field_upload.width == domain.sample_width &&
      field_upload.height == domain.sample_height;
  if (valid_field && !field_hit) {
    const u64 field_bytes = static_cast<u64>(domain.sample_width) *
                            domain.sample_height * sizeof(GrassFieldSample);
    std::memcpy(slot.field.mapped, domain.samples, field_bytes);
    device_->FlushBuffer(slot.field, 0, field_bytes);
    if (domain.sample_revision != 0) {
      field_upload = {.source = domain.samples,
                      .revision = domain.sample_revision,
                      .width = domain.sample_width,
                      .height = domain.sample_height,
                      .valid = true};
    } else {
      field_upload.valid = false;
    }
  } else if (!valid_field) {
    field_upload.valid = false;
  }

  TypeUploadState& type_upload = slot.type_upload;
  const bool type_hit = domain.type_revision != 0 && type_upload.valid &&
                        type_upload.source == domain.types &&
                        type_upload.revision == domain.type_revision &&
                        type_upload.count == type_count;
  if (!type_hit) {
    const u64 type_bytes = static_cast<u64>(type_count) * sizeof(GrassType);
    std::memcpy(slot.types.mapped, domain.types, type_bytes);
    device_->FlushBuffer(slot.types, 0, type_bytes);
    if (domain.type_revision != 0) {
      type_upload = {.source = domain.types,
                     .revision = domain.type_revision,
                     .count = type_count,
                     .valid = true};
    } else {
      type_upload.valid = false;
    }
  }

  const u32 interaction_count =
      std::min(static_cast<u32>(interactions.size()), kMaxInteractions);
  if (interaction_count > 0) {
    const u64 interaction_bytes =
        static_cast<u64>(interaction_count) * sizeof(GrassInteraction);
    std::memcpy(slot.interactions.mapped, interactions.data(), interaction_bytes);
    device_->FlushBuffer(slot.interactions, 0, interaction_bytes);
  }

  u32 surface_candidates = 0;
  u32 copied_surfaces = 0;
  const bool surface_packed_hit =
      surface_source_hit && surface_upload.packed_valid &&
      surface_upload.type_count == type_count;
  if (surface_packed_hit) {
    copied_surfaces = surface_upload.copied_surfaces;
    surface_candidates = surface_upload.surface_candidates;
  } else {
    auto* gpu_surfaces = static_cast<GpuSurface*>(slot.surfaces.mapped);
    for (u32 i = 0; i < surface_count; ++i) {
      const GrassSurfaceTriangle& src = domain.surfaces[i];
      if (!std::isfinite(src.density) || src.density <= 0.0f ||
          !std::isfinite(src.growth) || src.growth <= 0.0f)
        continue;
      u32 count = GrassSurfaceCandidateCount(src, settings.candidate_spacing);
      const u32 remaining = max_surface_candidates - surface_candidates;
      count = std::min(count, remaining);
      if (count == 0)
        continue;
      GpuSurface& dst = gpu_surfaces[copied_surfaces++];
      std::copy_n(src.p0, 3, dst.p0_density);
      dst.p0_density[3] = std::clamp(src.density, 0.0f, 1.0f);
      std::copy_n(src.p1, 3, dst.p1_growth);
      dst.p1_growth[3] = std::max(src.growth, 0.0f);
      std::copy_n(src.p2, 3, dst.p2_type);
      dst.p2_type[3] = std::bit_cast<f32>(std::min(src.type, type_count - 1));
      dst.meta[0] = surface_candidates;
      dst.meta[1] = count;
      dst.meta[2] = src.surface_id;
      dst.meta[3] = 0;
      surface_candidates += count;
      if (surface_candidates >= max_surface_candidates)
        break;
    }
    if (copied_surfaces > 0) {
      device_->FlushBuffer(slot.surfaces, 0,
                           static_cast<u64>(copied_surfaces) * sizeof(GpuSurface));
    }
    if (cache_surface_source) {
      surface_upload.type_count = type_count;
      surface_upload.copied_surfaces = copied_surfaces;
      surface_upload.surface_candidates = surface_candidates;
      surface_upload.packed_valid = true;
    } else {
      surface_upload.packed_valid = false;
    }
  }

  const u32 candidate_count = terrain_candidates + surface_candidates;
  if (candidate_count == 0)
    return false;

  const bool preserve_bend_history =
      bend_history_valid_ && bend_domain_seed_ == domain.seed &&
      bend_sample_source_ == domain.samples &&
      bend_surface_source_ == domain.surfaces;
  const f32 bend_extent = settings.stream_radius * 2.0f;
  const f32 bend_texel = bend_extent / static_cast<f32>(kBendResolution);
  const f32 bend_origin_x =
      std::floor((frame.camera_pos.x - bend_extent * 0.5f) / bend_texel) * bend_texel;
  const f32 bend_origin_z =
      std::floor((frame.camera_pos.z - bend_extent * 0.5f) / bend_texel) * bend_texel;
  const bool fields_overlap =
      preserve_bend_history && bend_origin_x < bend_origin_[0] + bend_extent_ &&
      bend_origin_x + bend_extent > bend_origin_[0] &&
      bend_origin_z < bend_origin_[1] + bend_extent_ &&
      bend_origin_z + bend_extent > bend_origin_[1];
  const f32 recovery_elapsed =
      preserve_bend_history && bend_history_active_
          ? (frame.time >= bend_last_update_time_
                 ? frame.time - bend_last_update_time_
                 : std::max(frame.delta_time, 0.0f))
          : 0.0f;
  f32 bend_max_strength = fields_overlap ? bend_max_strength_ : 0.0f;
  if (settings.bend_recovery_time > 0.0f) {
    bend_max_strength *=
        std::exp2(-recovery_elapsed / settings.bend_recovery_time);
  }
  const f32 bend_height_origin = std::floor(frame.camera_pos.y / 64.0f) * 64.0f;
  for (u32 i = 0; i < interaction_count; ++i) {
    const GrassInteraction& interaction = interactions[i];
    if (!AllFinite(interaction.position_radius, 4) ||
        !AllFinite(interaction.direction_strength, 4) ||
        interaction.position_radius[3] <= 0.0f ||
        std::fabs(interaction.direction_strength[3]) <= 1e-4f)
      continue;
    const f32 closest_x = std::clamp(interaction.position_radius[0], bend_origin_x,
                                     bend_origin_x + bend_extent);
    const f32 closest_z = std::clamp(interaction.position_radius[2], bend_origin_z,
                                     bend_origin_z + bend_extent);
    const f32 dx = interaction.position_radius[0] - closest_x;
    const f32 dz = interaction.position_radius[2] - closest_z;
    const f32 effective_radius = std::min(
        std::max(interaction.position_radius[3], bend_texel * 0.75f), bend_extent);
    if (dx * dx + dz * dz > effective_radius * effective_radius)
      continue;
    const f32 strength =
        std::min(std::fabs(interaction.direction_strength[3]), 2.0f);
    const f32 relative_height = interaction.position_radius[1] - bend_height_origin;
    const f32 radius = effective_radius;
    if (std::fabs(relative_height) * strength > 60000.0f ||
        radius * strength > 60000.0f)
      continue;
    bend_max_strength = std::max(bend_max_strength, strength);
  }
  const bool bend_active = bend_max_strength > 1e-3f;

  BendPush bend{};
  bend.field[0] = bend_origin_x;
  bend.field[1] = bend_origin_z;
  bend.field[2] = bend_extent;
  bend.field[3] = 1.0f / bend_extent;
  bend.prev_field[0] = bend_origin_[0];
  bend.prev_field[1] = bend_origin_[1];
  bend.prev_field[2] = bend_extent_;
  bend.prev_field[3] = 1.0f / bend_extent_;
  bend.params[0] = std::max(frame.delta_time, 0.0f);
  bend.params[1] = recovery_elapsed;
  bend.params[2] = settings.bend_recovery_time;
  bend.params[3] = static_cast<f32>(interaction_count);
  bend.height[0] = bend_height_origin;
  bend.height[1] = bend_height_origin_;
  bend.height[2] = fields_overlap && bend_history_active_ ? 1.0f : 0.0f;
  slot.bend = bend;
  slot.bend_write_index = bend_active ? bend_write_index_ ^ 1u : bend_write_index_;
  slot.bend_domain_seed = domain.seed;
  slot.bend_sample_source = domain.samples;
  slot.bend_surface_source = domain.surfaces;
  slot.bend_update_time = frame.time;
  slot.bend_max_strength = bend_max_strength;
  slot.bend_active = bend_active;

  GenerationPush common{};
  common.view_proj = frame.view_proj;
  common.field_origin_extent[0] = domain.origin_x;
  common.field_origin_extent[1] = domain.origin_z;
  common.field_origin_extent[2] = domain.extent_x;
  common.field_origin_extent[3] = domain.extent_z;
  common.field[0] = valid_field ? domain.sample_width : 0;
  common.field[1] = valid_field ? domain.sample_height : 0;
  common.field[2] = type_count;
  common.field[3] = domain.seed;
  common.camera_stream[0] = frame.camera_pos.x;
  common.camera_stream[1] = frame.camera_pos.y;
  common.camera_stream[2] = frame.camera_pos.z;
  common.camera_stream[3] = settings.stream_radius;
  common.placement[0] = settings.candidate_spacing;
  common.counts[1] = copied_surfaces;
  common.counts[2] = std::min(settings.max_blades, kMaxBlades);
  common.counts[3] = kMaxBlades;
  common.density_lod[0] = settings.density_lod_start;
  common.density_lod[1] = settings.density_lod_end;
  common.density_lod[2] = settings.far_density;
  common.density_lod[3] = settings.max_slope_cos;
  common.geometry_fade[0] = settings.geometry_lod_start;
  common.geometry_fade[1] = settings.geometry_lod_end;
  common.geometry_fade[2] = settings.fade_start;
  common.geometry_fade[3] = settings.fade_end;
  common.bend_field[0] = bend.field[0];
  common.bend_field[1] = bend.field[1];
  common.bend_field[2] = bend.height[0];
  common.bend_field[3] = bend_active ? bend.field[3] : 0.0f;

  slot.generation_count = 0;
  GenerationPush reset = common;
  reset.control[0] = 0;
  slot.generation[slot.generation_count++] = reset;
  auto append_surfaces = [&] {
    if (surface_candidates == 0)
      return;
    GenerationPush surfaces = common;
    surfaces.counts[0] = surface_candidates;
    surfaces.control[0] = 2;
    slot.generation[slot.generation_count++] = surfaces;
  };
  append_surfaces();
  for (u32 i = 0; i < ring_count; ++i) {
    const CandidateRing& ring = rings[i];
    GenerationPush generation = common;
    generation.placement[1] = ring.inner_radius;
    generation.placement[2] = ring.outer_radius;
    generation.placement[3] = ring.refinement_start;
    generation.grid[0] = ring.min_fine_x;
    generation.grid[1] = ring.min_fine_z;
    generation.grid[2] = static_cast<i32>(ring.cells);
    generation.grid[3] = static_cast<i32>(ring.cells);
    generation.counts[0] = ring.candidates;
    if (terrain_radius < settings.fade_end) {
      const f32 fade_length = settings.fade_end - settings.fade_start;
      generation.geometry_fade[2] = std::max(terrain_radius - fade_length, 0.0f);
      generation.geometry_fade[3] = terrain_radius;
    }
    generation.control[0] = 1;
    generation.control[1] = ring.stride;
    generation.control[2] = ring.next_stride;
    slot.generation[slot.generation_count++] = generation;
  }
  GenerationPush finalize = common;
  finalize.control[0] = 3;
  slot.generation[slot.generation_count++] = finalize;

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
  draw.control[1] = type_count;
  draw.control[2] = kNearSegments;
  draw.control[3] = kMaxBlades - 1u;
  slot.draw = draw;
  slot.active = true;
  return true;
}

void ProceduralGrass::AddGeneration(RenderGraph& graph, u32 frame_slot) {
  if (frame_slot >= kFramesInFlight || !slots_[frame_slot].active)
    return;
  Slot& slot = slots_[frame_slot];
  const BendPush bend = slot.bend;
  const u32 bend_write_index = slot.bend_write_index;
  const bool bend_active = slot.bend_active;
  const u32 bend_domain_seed = slot.bend_domain_seed;
  const GrassFieldSample* bend_sample_source = slot.bend_sample_source;
  const GrassSurfaceTriangle* bend_surface_source = slot.bend_surface_source;
  const f32 bend_update_time = slot.bend_update_time;
  const f32 bend_max_strength = slot.bend_max_strength;
  const std::array<GenerationPush, kMaxGenerationPhases> phases = slot.generation;
  const u32 phase_count = slot.generation_count;
  graph.AddPass(
      "procedural_grass_generate", [](RenderGraph::PassBuilder&) {},
      [this, frame_slot, bend, bend_write_index, bend_active, bend_domain_seed,
       bend_sample_source, bend_surface_source, bend_update_time,
       bend_max_strength, phases, phase_count](PassContext& ctx) {
        Slot& current = slots_[frame_slot];
        const GpuImage& bend_current = bend_fields_[bend_write_index];
        const GpuImage& bend_previous = bend_fields_[bend_write_index ^ 1u];
        const GpuImage& metadata_current = bend_metadata_[bend_write_index];
        const GpuImage& metadata_previous = bend_metadata_[bend_write_index ^ 1u];
        const GpuImage& confidence_current = bend_confidence_[bend_write_index];
        const GpuImage& confidence_previous = bend_confidence_[bend_write_index ^ 1u];
        if (bend_active) {
          const TextureBarrier barriers[] = {
              Transition(bend_current, ResourceState::kShaderReadCompute,
                         ResourceState::kGeneral),
              Transition(metadata_current, ResourceState::kShaderReadCompute,
                         ResourceState::kGeneral),
              Transition(confidence_current, ResourceState::kShaderReadCompute,
                         ResourceState::kGeneral),
          };
          ctx.cmd->TextureBarriers(barriers);
          ctx.cmd->BindPipeline(bend_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Combined(0, bend_previous.view, bend_sampler_),
                  Bind::StorageView(1, bend_current.view),
                  Bind::ByteBuffer(2, current.interactions),
                  Bind::Combined(3, metadata_previous.view, bend_sampler_),
                  Bind::StorageView(4, metadata_current.view),
                  Bind::Combined(5, confidence_previous.view, bend_sampler_),
                  Bind::StorageView(6, confidence_current.view)});
          ctx.cmd->Push(bend);
          ctx.cmd->Dispatch(kBendResolution / 8u, kBendResolution / 8u, 1);
          const TextureBarrier readable[] = {
              Transition(bend_current, ResourceState::kGeneral,
                         ResourceState::kShaderReadCompute),
              Transition(metadata_current, ResourceState::kGeneral,
                         ResourceState::kShaderReadCompute),
              Transition(confidence_current, ResourceState::kGeneral,
                         ResourceState::kShaderReadCompute),
          };
          ctx.cmd->TextureBarriers(readable);
        }

        ctx.cmd->BindPipeline(generate_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::ByteBuffer(0, current.field), Bind::ByteBuffer(1, current.types),
                Bind::ByteBuffer(2, current.surfaces),
                 Bind::StorageBuffer(3, current.instances),
                 Bind::StorageBuffer(4, current.args),
                 Bind::StorageBuffer(5, current.counters),
                 Bind::Combined(6, bend_current.view, bend_sampler_),
                 Bind::Combined(7, metadata_current.view, bend_sampler_),
                 Bind::Combined(8, confidence_current.view, bend_sampler_)});

        for (u32 i = 0; i < phase_count; ++i) {
          const GenerationPush& phase = phases[i];
          ctx.cmd->Push(phase);
          const bool generate = phase.control[0] == 1u || phase.control[0] == 2u;
          ctx.cmd->Dispatch(generate ? (phase.counts[0] + 63u) / 64u : 1u, 1, 1);
          if (i + 1u < phase_count) {
            ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite,
                                   BarrierScope::kComputeReadWrite);
          }
        }
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kIndirectArgs);
        bend_write_index_ = bend_write_index;
        bend_origin_[0] = bend.field[0];
        bend_origin_[1] = bend.field[1];
        bend_extent_ = bend.field[2];
        bend_height_origin_ = bend.height[0];
        bend_last_update_time_ = bend_update_time;
        bend_max_strength_ = bend_max_strength;
        bend_domain_seed_ = bend_domain_seed;
        bend_sample_source_ = bend_sample_source;
        bend_surface_source_ = bend_surface_source;
        bend_history_valid_ = true;
        bend_history_active_ = bend_active;
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
      0, {Bind::ByteBuffer(0, slot.instances), Bind::ByteBuffer(2, slot.types)});
  DrawPush draw = slot.draw;
  draw.control[2] = kNearSegments;
  cmd.Push(draw);
  cmd.DrawIndirectCount(slot.args, 0, slot.counters, 16, 1, 16);
  draw.control[2] = kFarSegments;
  cmd.Push(draw);
  cmd.DrawIndirectCount(slot.args, 16, slot.counters, 20, 1, 16);
}

void ProceduralGrass::DrawPrepass(CommandList& cmd, u32 frame_slot, u32 samples) const {
  Draw(cmd, frame_slot, prepass_pipelines_[PipelineIndex(samples)]);
}

void ProceduralGrass::DrawScene(CommandList& cmd, u32 frame_slot, u32 samples) const {
  Draw(cmd, frame_slot, scene_pipelines_[PipelineIndex(samples)]);
}

}  // namespace rx::render
