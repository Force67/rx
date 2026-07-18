#ifndef RX_RENDER_PROCEDURAL_GRASS_H_
#define RX_RENDER_PROCEDURAL_GRASS_H_

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

// One semantic heightfield texel. The renderer reconstructs blades from this
// compact description instead of storing authored blade transforms.
struct alignas(16) GrassFieldSample {
  f32 height = 0.0f;
  f32 density = 1.0f;
  u32 type = 0;
  f32 growth = 1.0f;
};
static_assert(sizeof(GrassFieldSample) == 16);

// Artist-facing blade family. Colors are linear; dimensions are in world units.
// The layout is mirrored by the generation, vertex and pixel shaders.
struct alignas(16) GrassType {
  f32 base_color[4] = {0.08f, 0.22f, 0.035f, 0.12f};  // a = clump color variation
  f32 tip_color[4] = {0.34f, 0.58f, 0.10f, 0.35f};    // a = translucency
  f32 dimensions[4] = {0.45f, 0.9f, 0.018f, 0.04f};   // height min/max, width min/max
  f32 shape[4] = {0.05f, 0.28f, 0.22f, 0.08f};        // tilt min/max, bend, side curve
  f32 material[4] = {0.78f, 1.0f, 3.5f, 0.28f};  // roughness, wind, clump size, view bias
};
static_assert(sizeof(GrassType) == 80);

// A growable triangle supplied by an authored or placed mesh. Surface IDs must
// remain stable while the object is loaded; they seed deterministic candidates.
// This permits grass on rocks and soil meshes, including overlapping surfaces
// that cannot be represented by one heightfield.
struct alignas(16) GrassSurfaceTriangle {
  f32 p0[3] = {0, 0, 0};
  f32 density = 1.0f;
  f32 p1[3] = {0, 0, 0};
  f32 growth = 1.0f;
  f32 p2[3] = {0, 0, 0};
  u32 type = 0;
  u32 surface_id = 0;
  u32 pad[3] = {};
};
static_assert(sizeof(GrassSurfaceTriangle) == 64);

// A bounded local displacement source. Zero direction pushes radially away
// from position; otherwise direction is projected onto each growing surface.
struct alignas(16) GrassInteraction {
  f32 position_radius[4] = {0, 0, 0, 1};
  f32 direction_strength[4] = {0, 0, 0, 1};
};
static_assert(sizeof(GrassInteraction) == 32);

// Generation/streaming/LOD controls are deliberately independent. A game can
// change density without changing stream tiles or blade tessellation.
struct GrassGenerationSettings {
  f32 candidate_spacing = 0.42f;
  f32 stream_tile_size = 16.0f;
  f32 stream_radius = 80.0f;
  f32 density_lod_start = 35.0f;
  f32 density_lod_end = 75.0f;
  f32 far_density = 0.28f;
  f32 geometry_lod_start = 22.0f;
  f32 geometry_lod_end = 58.0f;
  f32 fade_start = 72.0f;
  f32 fade_end = 80.0f;
  f32 max_slope_cos = 0.55f;
  u32 max_blades = 131072;
};

inline GrassGenerationSettings SanitizeGrassSettings(GrassGenerationSettings settings) {
  const GrassGenerationSettings defaults;
  auto finite_or = [](f32 value, f32 fallback) {
    return std::isfinite(value) ? value : fallback;
  };
  settings.candidate_spacing =
      finite_or(settings.candidate_spacing, defaults.candidate_spacing);
  settings.stream_tile_size =
      finite_or(settings.stream_tile_size, defaults.stream_tile_size);
  settings.stream_radius = finite_or(settings.stream_radius, defaults.stream_radius);
  settings.density_lod_start =
      finite_or(settings.density_lod_start, defaults.density_lod_start);
  settings.density_lod_end =
      finite_or(settings.density_lod_end, defaults.density_lod_end);
  settings.far_density = finite_or(settings.far_density, defaults.far_density);
  settings.geometry_lod_start =
      finite_or(settings.geometry_lod_start, defaults.geometry_lod_start);
  settings.geometry_lod_end =
      finite_or(settings.geometry_lod_end, defaults.geometry_lod_end);
  settings.fade_start = finite_or(settings.fade_start, defaults.fade_start);
  settings.fade_end = finite_or(settings.fade_end, defaults.fade_end);
  settings.max_slope_cos = finite_or(settings.max_slope_cos, defaults.max_slope_cos);

  settings.candidate_spacing = std::clamp(settings.candidate_spacing, 0.08f, 8.0f);
  settings.stream_tile_size = std::clamp(settings.stream_tile_size, 1.0f, 512.0f);
  settings.stream_radius =
      std::clamp(settings.stream_radius, settings.candidate_spacing, 512.0f);
  settings.density_lod_start = std::max(settings.density_lod_start, 0.0f);
  settings.density_lod_end = std::max(
      settings.density_lod_end, settings.density_lod_start + settings.candidate_spacing);
  settings.far_density = std::clamp(settings.far_density, 0.0f, 1.0f);
  settings.geometry_lod_start = std::max(settings.geometry_lod_start, 0.0f);
  settings.geometry_lod_end =
      std::max(settings.geometry_lod_end,
               settings.geometry_lod_start + settings.candidate_spacing);
  const f32 latest_fade_start =
      std::max(settings.stream_radius - settings.candidate_spacing, 0.0f);
  settings.fade_start = std::clamp(settings.fade_start, 0.0f, latest_fade_start);
  settings.fade_end =
      std::clamp(settings.fade_end, settings.fade_start + settings.candidate_spacing,
                 settings.stream_radius);
  settings.max_slope_cos = std::clamp(settings.max_slope_cos, 0.0f, 1.0f);
  settings.max_blades = std::clamp(settings.max_blades, 1u, 262144u);
  return settings;
}

inline u32 GrassSurfaceCandidateCount(const GrassSurfaceTriangle& triangle,
                                      f32 candidate_spacing) {
  const Vec3 a{triangle.p0[0], triangle.p0[1], triangle.p0[2]};
  const Vec3 b{triangle.p1[0], triangle.p1[1], triangle.p1[2]};
  const Vec3 c{triangle.p2[0], triangle.p2[1], triangle.p2[2]};
  const f32 area = 0.5f * Length(Cross(b - a, c - a));
  const f32 spacing = std::clamp(
      std::isfinite(candidate_spacing) ? candidate_spacing : 0.42f, 0.08f, 8.0f);
  if (!std::isfinite(area) || area <= 1e-6f)
    return 0;
  const f64 candidates =
      std::ceil(static_cast<f64>(area) / (static_cast<f64>(spacing) * spacing));
  return static_cast<u32>(
      std::clamp(candidates, 1.0, static_cast<f64>(std::numeric_limits<u32>::max())));
}

// Non-owning semantic field submitted for one RenderFrame call. The heightfield
// is optional when growable surface triangles are present.
struct GrassDomain {
  const GrassFieldSample* samples = nullptr;
  u32 sample_width = 0;
  u32 sample_height = 0;
  f32 origin_x = 0.0f;
  f32 origin_z = 0.0f;
  f32 extent_x = 1.0f;
  f32 extent_z = 1.0f;
  const GrassType* types = nullptr;
  u32 type_count = 0;
  const GrassSurfaceTriangle* surfaces = nullptr;
  u32 surface_count = 0;
  u32 seed = 1;
  GrassGenerationSettings settings;
};

// Compute-generated cubic-Bezier blade field. It owns fixed-capacity per-frame
// arenas; Prepare uploads semantic inputs, AddGeneration culls/appends blades,
// and the two draw methods participate in the renderer's depth/motion prepass
// and opaque lighting pass.
class ProceduralGrass {
 public:
  struct Frame {
    Mat4 view_proj = Mat4::Identity();
    Mat4 prev_view_proj = Mat4::Identity();
    Vec3 camera_pos{};
    Vec3 sun_direction{0, -1, 0};
    Vec3 sun_color{1, 1, 1};
    f32 sun_intensity = 4.0f;
    f32 ambient = 0.08f;
    f32 time = 0.0f;
    f32 delta_time = 1.0f / 60.0f;
    f32 jitter[2] = {0, 0};
    f32 wind_speed = 6.0f;
    f32 wind_yaw = 0.0f;
    f32 gustiness = 0.5f;
  };

  static constexpr u32 kMaxFieldDimension = 256;
  static constexpr u32 kMaxTypes = 8;
  static constexpr u32 kMaxInteractions = 16;
  static constexpr u32 kMaxSurfaces = 2048;
  static constexpr u32 kMaxCandidates = 1u << 20;
  static constexpr u32 kMaxBlades = 1u << 18;
  static constexpr u32 kVerticesPerBlade = 42;  // seven cubic-curve segments

  bool Initialize(Device& device,
                  Format scene_color,
                  Format motion,
                  Format normal,
                  Format skin_diffuse,
                  Format depth);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(generate_pipeline_); }
  bool EnsureSampleCount(Device& device, u32 samples);

  bool Prepare(const GrassDomain& domain,
               std::span<const GrassInteraction> interactions,
               const Frame& frame,
               u32 frame_slot);
  void AddGeneration(RenderGraph& graph, u32 frame_slot);
  void DrawPrepass(CommandList& cmd, u32 frame_slot, u32 samples) const;
  void DrawScene(CommandList& cmd, u32 frame_slot, u32 samples) const;

 private:
  static constexpr u32 kFramesInFlight = Device::kMaxFramesInFlight;

  struct alignas(16) GenerationPush {
    Mat4 view_proj;
    f32 field_origin_extent[4];
    u32 field[4];
    f32 camera_stream[4];
    f32 placement[4];
    i32 grid[4];
    u32 counts[4];
    f32 density_lod[4];
    f32 geometry_fade[4];
    u32 control[4];
  };
  static_assert(sizeof(GenerationPush) == 208);

  struct alignas(16) DrawPush {
    Mat4 view_proj;
    Mat4 prev_view_proj;
    f32 camera_time[4];
    f32 sun_direction_intensity[4];
    f32 sun_color_ambient[4];
    f32 wind[4];
    f32 jitter_lod[4];
    u32 control[4];
  };
  static_assert(sizeof(DrawPush) == 224);

  struct Slot {
    GpuBuffer field;
    GpuBuffer types;
    GpuBuffer surfaces;
    GpuBuffer interactions;
    GpuBuffer instances;
    GpuBuffer args;
    GpuBuffer counters;
    GenerationPush generation{};
    DrawPush draw{};
    bool active = false;
  };

  static u32 PipelineIndex(u32 samples);
  bool CreateDrawPipelines(Device& device, u32 samples);
  bool EnsureBuffers();
  void Draw(CommandList& cmd, u32 frame_slot, PipelineHandle pipeline) const;

  Device* device_ = nullptr;
  Format scene_color_format_ = Format::kUnknown;
  Format motion_format_ = Format::kUnknown;
  Format normal_format_ = Format::kUnknown;
  Format skin_diffuse_format_ = Format::kUnknown;
  Format depth_format_ = Format::kUnknown;
  bool allocation_failed_ = false;
  u32 failed_sample_mask_ = 0;
  PipelineHandle generate_pipeline_;
  PipelineHandle prepass_pipelines_[4] = {};
  PipelineHandle scene_pipelines_[4] = {};
  Slot slots_[kFramesInFlight];
};

}  // namespace rx::render

#endif  // RX_RENDER_PROCEDURAL_GRASS_H_
