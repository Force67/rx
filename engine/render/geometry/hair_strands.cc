#include "render/geometry/hair_strands.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "shaders/hair_dom_ps_hlsl.h"
#include "shaders/hair_ps_hlsl.h"
#include "shaders/hair_vs_hlsl.h"

namespace rx::render {
namespace {

constexpr u32 kPointsPerStrand = kGroomPointsPerStrand;

// One push block for the lit draw and both deep-opacity-map passes: they share
// hair.vs, and a SPIR-V push block has to match across the stages of a pipeline.
// Mirrors DrawPush in hair.vs.hlsl / hair.ps.hlsl / hair_dom.ps.hlsl.
// Exactly the 128-byte push floor the RHI guarantees, so it stays portable.
// Everything else - the fibre material, the light frustum, the frame's ambient -
// lives in uniform buffers, which is also where it belongs: the material is
// per groom and changes rarely, the frustum is per frame and shared.
struct DrawPush {
  Mat4 view_proj;
  f32 camera[4];     // xyz eye, w width
  f32 sun[4];        // xyz travel, w intensity
  f32 sun_color[4];  // rgb, w clump radius
  f32 tint[4];       // rgb tint, w children count
};

// Mirrors HairTransmittanceParams in shaders/geometry/hair_transmittance.hlsli.
struct VolumeParams {
  Mat4 light_view_proj;
  f32 depth_range = 1.0f;
  f32 layer_depth = 0.18f;
  f32 fibre_scale = 1.0f;
  f32 enabled = 0.0f;
  f32 ambient[4] = {0, 0, 0, 0};
  f32 debug[4] = {0, 0, 0, 0};
};

// Mirrors HairMaterial in hair.ps.hlsl. Written when a groom's material or tier
// changes, not per frame, so the tier reduction is applied exactly once.
struct GroomMaterial {
  f32 sigma_a[3] = {0, 0, 0};
  f32 beta_m = 0.3f;
  f32 beta_n = 0.3f;
  f32 alpha = 0.0f;
  f32 eta = 1.55f;
  f32 density = 1.0f;
  f32 scatter_scale = 1.0f;
  f32 caps = 0.0f;
  f32 color_reference_depth = 6.0f;
  f32 pad = 0.0f;
};

// Mirrors the kHairCap* constants in hair.ps.hlsl.
constexpr u32 kCapDualScatter = 1u;
constexpr u32 kCapVolume = 2u;
constexpr u32 kCapPerFragmentH = 4u;
constexpr u32 kCapTilt = 8u;
constexpr u32 kCapAuthoredColor = 16u;

u32 PackCaps(const HairTierCaps& caps) {
  return (caps.dual_scattering ? kCapDualScatter : 0u) |
         (caps.transmittance_volume ? kCapVolume : 0u) |
         (caps.per_fragment_h ? kCapPerFragmentH : 0u) | (caps.tilted_lobes ? kCapTilt : 0u);
}

constexpr Format kDomFormat = Format::kRGBA16Float;
constexpr Format kDomDepthFormat = Format::kD32Float;

ByteSpan Span(const void* data, size_t bytes) {
  return ByteSpan(static_cast<const u8*>(data), bytes);
}

constexpr u32 kAllSlotsStale = (1u << HairStrands::kFramesInFlight) - 1;

}  // namespace

bool HairStrands::Initialize(Device& device, Format color_format, Format depth_format) {
  device_ = &device;
  draw_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_hair_vs_hlsl),
      .fragment = RX_SHADER(k_hair_ps_hlsl),
      .raster = {.cull = CullMode::kNone},  // ribbons flip with the view
      .depth = {.test = true, .write = true, .compare = CompareOp::kGreaterEqual,
                .format = depth_format},
      .color_formats = {color_format},
      // The fragment stage reads the transmittance volume, so the set spans
      // both stages now.
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kCombinedTextureSampler},
                          {4, BindingType::kUniformBuffer},
                          {5, BindingType::kUniformBuffer}},
                .stages = kShaderStageVertex | kShaderStageFragment}},
      .push_constant_size = PushSize<DrawPush>(),
      .debug_name = "hair_draw",
  });
  if (!draw_pipeline_) {
    RX_ERROR("hair pipeline creation failed");
    return false;
  }

  // Deep opacity map, pass one: front-most fibre depth from the sun. No
  // fragment shader - the depth is the whole output.
  depth_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_hair_vs_hlsl),
      .fragment = ShaderBlob{},
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true, .write = true, .compare = CompareOp::kLess,
                .format = kDomDepthFormat},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer}, {1, BindingType::kStorageBuffer}},
                .stages = kShaderStageVertex}},
      .push_constant_size = PushSize<DrawPush>(),
      .debug_name = "hair_dom_depth",
  });
  // Pass two: additive fibre counts, depth test OFF. Every fibre along the ray
  // has to be counted, including the ones the front one occludes - the whole
  // point is what is BEHIND the first strand.
  dom_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_hair_vs_hlsl),
      .fragment = RX_SHADER(k_hair_dom_ps_hlsl),
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = false, .write = false, .format = kDomDepthFormat},
      .color_formats = {kDomFormat},
      .blend = {BlendMode::kAdditive},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kCombinedTextureSampler},
                          {3, BindingType::kUniformBuffer}},
                .stages = kShaderStageVertex | kShaderStageFragment}},
      .push_constant_size = PushSize<DrawPush>(),
      .debug_name = "hair_dom",
  });
  if (!depth_pipeline_ || !dom_pipeline_) {
    // Hair still renders without the volume, just without an interior; say so
    // rather than failing the renderer.
    RX_WARN("hair transmittance volume unavailable; grooms will shade without self-shadowing");
  }

  volume_sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                       .mag_filter = Filter::kLinear,
                                       .address_u = AddressMode::kClampToEdge,
                                       .address_v = AddressMode::kClampToEdge});
  front_depth_ = device.CreateImage2D(kDomDepthFormat,
                                      {kTransmittanceResolution, kTransmittanceResolution},
                                      kTextureUsageDepthTarget | kTextureUsageSampled);
  dom_ = device.CreateImage2D(kDomFormat, {kTransmittanceResolution, kTransmittanceResolution},
                              kTextureUsageColorTarget | kTextureUsageSampled);
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    volume_params_[i] = device.CreateBuffer(sizeof(VolumeParams), kBufferUsageUniform, true);
  }
  // Park both images in a readable state up front. A frame that skips the
  // volume passes still binds them, and a descriptor pointing at an image in
  // kUndefined is a validation error rather than a black texture.
  if (front_depth_ && dom_) {
    device.ImmediateSubmit([&](CommandList& cmd) {
      cmd.Barrier(Transition(front_depth_, ResourceState::kUndefined,
                             ResourceState::kShaderReadFragment));
      cmd.Barrier(
          Transition(dom_, ResourceState::kUndefined, ResourceState::kShaderReadFragment));
    });
    front_depth_state_ = ResourceState::kShaderReadFragment;
    dom_state_ = ResourceState::kShaderReadFragment;
  }
  return true;
}

void HairStrands::WriteGroomMaterial(Groom& g) {
  if (!g.material.mapped) return;
  // The tier reduction is applied HERE, once, rather than per frame in the
  // draw: a tier is a property of the material as shipped, and applying it in
  // two places is how the two drift apart.
  HairSurfaceParameters hair = g.hair;
  HairTierCaps caps = HairTierApply(g.tier, hair);
  if (!depth_pipeline_ || !dom_pipeline_) caps.transmittance_volume = false;
  GroomMaterial m;
  std::memcpy(m.sigma_a, hair.sigma_a, sizeof(f32) * 3);
  m.beta_m = hair.beta_m;
  m.beta_n = hair.beta_n;
  m.alpha = hair.alpha;
  m.eta = hair.eta;
  m.density = hair.density;
  m.scatter_scale = hair.scatter_scale;
  m.color_reference_depth = hair.color_reference_depth;
  m.caps = static_cast<f32>(PackCaps(caps) |
                            (hair.color_mode == HairColorMode::kAuthored ? kCapAuthoredColor
                                                                        : 0u));
  std::memcpy(g.material.mapped, &m, sizeof(m));
}

void HairStrands::SetGroomHair(u32 id, const HairSurfaceParameters& params) {
  if (Groom* g = Find(id)) {
    g->hair = params;
    WriteGroomMaterial(*g);
  }
}

void HairStrands::SetGroomTier(u32 id, HairTier tier) {
  if (Groom* g = Find(id)) {
    g->tier = tier;
    WriteGroomMaterial(*g);
  }
}

bool HairStrands::WorldBounds(Vec3* lo, Vec3* hi) const {
  bool any = false;
  Vec3 mn{1e30f, 1e30f, 1e30f};
  Vec3 mx{-1e30f, -1e30f, -1e30f};
  for (const Groom& g : grooms_) {
    if (!g.alive) continue;
    for (const HairPoint& p : g.host_points) {
      mn = {std::min(mn.x, p.pos[0]), std::min(mn.y, p.pos[1]), std::min(mn.z, p.pos[2])};
      mx = {std::max(mx.x, p.pos[0]), std::max(mx.y, p.pos[1]), std::max(mx.z, p.pos[2])};
      any = true;
    }
  }
  if (!any) return false;
  *lo = mn;
  *hi = mx;
  return true;
}

HairStrands::TransmittanceBinding HairStrands::transmittance() const {
  TransmittanceBinding b;
  if (!volume_valid_) return b;
  b.front_depth = front_depth_.view;
  b.layers = dom_.view;
  b.params = &volume_params_[volume_slot_];
  b.sampler = volume_sampler_;
  return b;
}

void HairStrands::Destroy(Device& device) {
  if (draw_pipeline_) device.DestroyPipeline(draw_pipeline_);
  if (depth_pipeline_) device.DestroyPipeline(depth_pipeline_);
  if (dom_pipeline_) device.DestroyPipeline(dom_pipeline_);
  draw_pipeline_ = {};
  depth_pipeline_ = {};
  dom_pipeline_ = {};
  device.DestroyImage(front_depth_);
  device.DestroyImage(dom_);
  front_depth_ = {};
  dom_ = {};
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    device.DestroyBuffer(volume_params_[i]);
    volume_params_[i] = {};
  }
  volume_valid_ = false;
  device_ = nullptr;
  for (Groom& g : grooms_) {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
      if (g.points[i]) device.DestroyBuffer(g.points[i]);
      g.points[i] = {};
    }
    for (GpuBuffer* b : {&g.colors, &g.indices, &g.material}) {
      if (*b) device.DestroyBuffer(*b);
      *b = {};
    }
    g.alive = false;
  }
  grooms_.clear();
}

bool HairStrands::active() const {
  for (const Groom& g : grooms_) {
    if (g.alive) return true;
  }
  return false;
}

HairStrands::Groom* HairStrands::Find(u32 id) {
  for (Groom& g : grooms_) {
    if (g.alive && g.id == id) return &g;
  }
  return nullptr;
}

u32 HairStrands::Upload(Device& device, const GroomData& data, const GroomParams& params,
                        const Mat4& transform) {
  if (data.guide_count == 0 || data.points.size() < data.guide_count * kPointsPerStrand * 3) {
    return 0;
  }
  const u32 n = data.guide_count;
  const u32 children = params.children_per_guide == 0 ? 1 : params.children_per_guide;

  Groom g;
  g.host_points.resize(static_cast<size_t>(n) * kPointsPerStrand);
  g.local_points.resize(static_cast<size_t>(n) * kPointsPerStrand * 3);
  std::memcpy(g.local_points.data(), data.points.data(),
              g.local_points.size() * sizeof(f32));
  base::Vector<f32> host_colors;
  host_colors.resize(static_cast<size_t>(n) * 4);

  for (u32 s = 0; s < n; ++s) {
    for (u32 k = 0; k < kPointsPerStrand; ++k) {
      size_t li = (static_cast<size_t>(s) * kPointsPerStrand + k);
      const f32* lp = &data.points[li * 3];
      Vec3 world = TransformPoint(transform, Vec3{lp[0], lp[1], lp[2]});
      HairPoint& hp = g.host_points[li];
      hp.pos[0] = world.x;
      hp.pos[1] = world.y;
      hp.pos[2] = world.z;
      hp.pos[3] = 0.0f;
      hp.prev[0] = world.x;
      hp.prev[1] = world.y;
      hp.prev[2] = world.z;
      hp.prev[3] = 0.0f;
    }
    host_colors[s * 4 + 0] = data.colors[s * 3 + 0];
    host_colors[s * 4 + 1] = data.colors[s * 3 + 1];
    host_colors[s * 4 + 2] = data.colors[s * 3 + 2];
    host_colors[s * 4 + 3] = 1.0f;
  }

  // Ribbon topology over n * children rendered strands.
  base::Vector<u32> idx;
  const u32 blocks = n * children;
  idx.reserve(static_cast<size_t>(blocks) * (kPointsPerStrand - 1) * 6);
  for (u32 b = 0; b < blocks; ++b) {
    u32 vbase = b * kPointsPerStrand * 2;
    for (u32 i = 0; i + 1 < kPointsPerStrand; ++i) {
      u32 v0 = vbase + i * 2, v1 = v0 + 1, v2 = v0 + 2, v3 = v0 + 3;
      idx.push_back(v0); idx.push_back(v2); idx.push_back(v1);
      idx.push_back(v1); idx.push_back(v2); idx.push_back(v3);
    }
  }

  // Host-visible per-frame ring: the CPU simulation feed rewrites a slot only
  // when SetGroomPoints marked it stale, so an unfed groom costs no copies.
  const u64 points_bytes = g.host_points.size() * sizeof(HairPoint);
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    g.points[i] = device.CreateBuffer(points_bytes, kBufferUsageStorage, true);
    if (!g.points[i].mapped) {
      for (u32 j = 0; j <= i; ++j) {
        if (g.points[j]) device.DestroyBuffer(g.points[j]);
      }
      return 0;
    }
    std::memcpy(g.points[i].mapped, g.host_points.data(), points_bytes);
  }
  g.colors = device.CreateBufferWithData(
      Span(host_colors.data(), host_colors.size() * sizeof(f32)), kBufferUsageStorage);
  g.indices = device.CreateBufferWithData(Span(idx.data(), idx.size() * sizeof(u32)),
                                          kBufferUsageIndex);
  g.guide_count = n;
  g.children = children;
  g.index_count = static_cast<u32>(idx.size());
  g.strand_width = params.strand_width;
  g.clump_radius = params.clump_radius;
  g.transform = transform;
  g.collision_center = data.collision_center;
  g.collision_radius = data.collision_radius;
  g.tint = params.tint;
  g.material = device.CreateBuffer(sizeof(GroomMaterial), kBufferUsageUniform, true);
  // A groom with no authored fibre starts brown rather than at the struct's
  // zero absorption, which would be a colourless fibre nobody wants to see.
  g.hair = HairPresetParams(HairPreset::kBrown);
  g.id = next_id_++;
  g.alive = true;
  grooms_.push_back(std::move(g));
  WriteGroomMaterial(grooms_.back());
  RX_INFO("hair: groom {} uploaded, {} guides x{} children, {} ribbon tris", grooms_.back().id, n,
           children, grooms_.back().index_count / 3);
  return grooms_.back().id;
}

u32 HairStrands::CreateGroom(Device& device, const GroomData& data, const GroomParams& params,
                             const Mat4& transform) {
  return Upload(device, data, params, transform);
}

void HairStrands::SetGroomTransform(u32 id, const Mat4& transform) {
  Groom* g = Find(id);
  if (!g) return;
  g->transform = transform;
  if (g->fed) return;  // node positions are owned by the simulation feed
  for (size_t i = 0; i < g->host_points.size(); ++i) {
    const f32* lp = &g->local_points[i * 3];
    Vec3 world = TransformPoint(transform, Vec3{lp[0], lp[1], lp[2]});
    HairPoint& hp = g->host_points[i];
    hp.pos[0] = world.x;
    hp.pos[1] = world.y;
    hp.pos[2] = world.z;
  }
  g->stale = kAllSlotsStale;
}

void HairStrands::SetGroomTint(u32 id, const Vec3& tint) {
  if (Groom* g = Find(id)) g->tint = tint;
}

void HairStrands::SetGroomPoints(u32 id, const f32* positions, u32 count) {
  Groom* g = Find(id);
  if (!g || !positions) return;
  const size_t nodes =
      std::min<size_t>(static_cast<size_t>(count) / 3, g->host_points.size());
  for (size_t i = 0; i < nodes; ++i) {
    HairPoint& hp = g->host_points[i];
    hp.prev[0] = hp.pos[0];
    hp.prev[1] = hp.pos[1];
    hp.prev[2] = hp.pos[2];
    hp.pos[0] = positions[i * 3 + 0];
    hp.pos[1] = positions[i * 3 + 1];
    hp.pos[2] = positions[i * 3 + 2];
  }
  g->fed = true;
  g->stale = kAllSlotsStale;
}

bool HairStrands::GroomHead(u32 id, Vec3* center, f32* radius) {
  Groom* g = Find(id);
  if (!g) return false;
  *center = TransformPoint(g->transform, g->collision_center);
  *radius = g->collision_radius;
  return true;
}

void HairStrands::DestroyGroom(Device& device, u32 id) {
  Groom* g = Find(id);
  if (!g) return;
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    if (g->points[i]) device.DestroyBuffer(g->points[i]);
    g->points[i] = {};
  }
  for (GpuBuffer* b : {&g->colors, &g->indices, &g->material}) {
    if (*b) device.DestroyBuffer(*b);
    *b = {};
  }
  g->alive = false;
}

void HairStrands::SeedCap(Device& device, const Vec3& head_center, f32 head_radius,
                          u32 strand_count, f32 strand_length) {
  // Fibonacci-distributed roots over the upper hemisphere, in a groom-local
  // frame (scalp at origin); a simulation feed relaxes them under gravity.
  GroomData data;
  data.guide_count = strand_count;
  data.points.reserve(static_cast<size_t>(strand_count) * kPointsPerStrand * 3);
  data.roots.reserve(static_cast<size_t>(strand_count) * 3);
  data.colors.reserve(static_cast<size_t>(strand_count) * 3);
  const f32 golden = 2.399963f;
  const f32 segment = strand_length / (kPointsPerStrand - 1);
  for (u32 s = 0; s < strand_count; ++s) {
    f32 t = (static_cast<f32>(s) + 0.5f) / strand_count;
    f32 y = 0.45f + 0.55f * t;
    f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    f32 a = golden * static_cast<f32>(s);
    Vec3 nrm{r * std::cos(a), y, r * std::sin(a)};
    Vec3 root = nrm * head_radius;  // local: scalp at origin
    for (u32 i = 0; i < kPointsPerStrand; ++i) {
      Vec3 p = root + nrm * (segment * static_cast<f32>(i));
      data.points.push_back(p.x);
      data.points.push_back(p.y);
      data.points.push_back(p.z);
      if (i == 0) {
        data.roots.push_back(root.x);
        data.roots.push_back(root.y);
        data.roots.push_back(root.z);
      }
    }
    data.colors.push_back(0.35f);
    data.colors.push_back(0.22f);
    data.colors.push_back(0.11f);
  }
  data.collision_center = {0, 0, 0};
  data.collision_radius = head_radius * 1.02f;

  GroomParams params;
  params.children_per_guide = 1;
  params.strand_width = 0.0011f;
  params.clump_radius = 0.0f;
  params.tint = {1, 1, 1};
  Upload(device, data, params, MakeTranslation(head_center));
}

namespace {

// Fills the per-groom half of the push. The lit draw and the DOM passes differ
// only in the matrix and the eye, so the material half is built once.
// The ribbon-expansion half of the push, identical for the lit draw and both
// volume passes.
void FillGroomPush(DrawPush& push, f32 width, f32 clump_radius, const Vec3& tint, u32 children) {
  push.camera[3] = width;
  push.sun_color[3] = clump_radius;
  push.tint[0] = tint.x;
  push.tint[1] = tint.y;
  push.tint[2] = tint.z;
  push.tint[3] = static_cast<f32>(children);
}

}  // namespace

void HairStrands::AddTransmittanceToGraph(RenderGraph& graph, const Frame& frame,
                                          u32 frame_slot) {
  volume_valid_ = false;
  volume_slot_ = frame_slot % kFramesInFlight;
  // The lit pass reads the frame's ambient out of this buffer whether or not
  // the volume runs, so it is published before any early return.
  if (volume_params_[volume_slot_].mapped) {
    VolumeParams idle;
    idle.layer_depth = frame.transmittance_depth;
    idle.fibre_scale = frame.fibre_scale;
    idle.ambient[0] = frame.ambient.x;
    idle.ambient[1] = frame.ambient.y;
    idle.ambient[2] = frame.ambient.z;
    idle.ambient[3] = frame.shadow_density;
    idle.debug[0] = static_cast<f32>(frame.debug_view);
    std::memcpy(volume_params_[volume_slot_].mapped, &idle, sizeof(idle));
  }
  if (!active() || !frame.transmittance || !depth_pipeline_ || !dom_pipeline_ || !device_) return;

  Vec3 lo, hi;
  if (!WorldBounds(&lo, &hi)) return;

  // Fit an orthographic light frustum to the grooms. The volume follows the
  // hair rather than the world, which is what lets 1024 texels resolve
  // individual strands on a head.
  const Vec3 centre{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
  const Vec3 extent_v{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
  const f32 radius =
      0.5f * std::sqrt(extent_v.x * extent_v.x + extent_v.y * extent_v.y +
                       extent_v.z * extent_v.z) +
      0.02f;
  const Vec3 dir = Normalize(frame.sun_direction);
  const Vec3 eye{centre.x - dir.x * (radius * 2.0f), centre.y - dir.y * (radius * 2.0f),
                 centre.z - dir.z * (radius * 2.0f)};
  const Vec3 up_ref = std::abs(dir.y) > 0.99f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
  const f32 depth_range = radius * 4.0f;
  const Mat4 light_vp = Orthographic(-radius, radius, -radius, radius, 0.0f, depth_range) *
                        LookAt(eye, centre, up_ref);

  const u32 slot = volume_slot_;
  if (volume_params_[slot].mapped) {
    VolumeParams params;
    params.light_view_proj = light_vp;
    params.depth_range = depth_range;
    params.layer_depth = frame.transmittance_depth;
    params.fibre_scale = frame.fibre_scale;
    params.enabled = 1.0f;
    params.ambient[0] = frame.ambient.x;
    params.ambient[1] = frame.ambient.y;
    params.ambient[2] = frame.ambient.z;
    // The scene-side shadow density rides in the spare slot: the forward pass
    // needs it and it is a property of the volume, not of a groom.
    params.ambient[3] = frame.shadow_density;
    params.debug[0] = static_cast<f32>(frame.debug_view);
    std::memcpy(volume_params_[slot].mapped, &params, sizeof(params));
  }

  // Publish this frame's simulated positions before either pass reads them.
  const u32 slot_bit = 1u << slot;
  for (Groom& g : grooms_) {
    if (!g.alive || !(g.stale & slot_bit)) continue;
    std::memcpy(g.points[slot].mapped, g.host_points.data(),
                g.host_points.size() * sizeof(HairPoint));
    g.stale &= ~slot_bit;
  }

  const Extent2D res{kTransmittanceResolution, kTransmittanceResolution};
  ResourceHandle front = graph.ImportImage("hair_dom_depth", front_depth_, &front_depth_state_);
  ResourceHandle layers = graph.ImportImage("hair_dom", dom_, &dom_state_);
  // Remembered so the lit draw can DECLARE the reads. Declaring them is the
  // synchronization: the graph derives the barriers from the declarations, and
  // an undeclared read of an image the same frame just wrote is the classic
  // works-on-my-GPU bug.
  volume_front_handle_ = front;
  volume_layers_handle_ = layers;

  auto record_grooms = [this, frame, light_vp, eye, depth_range, slot](PassContext& ctx,
                                                                       bool dom_pass) {
    for (Groom& g : grooms_) {
      if (!g.alive) continue;
      DrawPush push{};
      push.view_proj = light_vp;
      push.camera[0] = eye.x;
      push.camera[1] = eye.y;
      push.camera[2] = eye.z;
      FillGroomPush(push, g.strand_width, g.clump_radius, g.tint, g.children);
      if (dom_pass) {
        ctx.cmd->BindTransient(
            0, {Bind::StorageBuffer(0, g.points[slot], 0, g.points[slot].size),
                Bind::StorageBuffer(1, g.colors, 0, g.colors.size),
                Bind::Combined(2, front_depth_.view, volume_sampler_),
                Bind::Uniform(3, volume_params_[slot])});
      } else {
        ctx.cmd->BindTransient(
            0, {Bind::StorageBuffer(0, g.points[slot], 0, g.points[slot].size),
                Bind::StorageBuffer(1, g.colors, 0, g.colors.size)});
      }
      ctx.cmd->Push(push);
      ctx.cmd->BindIndexBuffer(g.indices, 0, IndexType::kUint32);
      ctx.cmd->DrawIndexed(g.index_count, 1, 0, 0, 0);
    }
  };

  graph.AddPass(
      "hair_dom_depth",
      [&](RenderGraph::PassBuilder& b) { b.Write(front, ResourceUsage::kDepthAttachment); },
      [this, front, res, record_grooms](PassContext& ctx) {
        DepthAttachment depth_att{.view = ctx.graph->image(front).view,
                                  .load = LoadOp::kClear,
                                  // Standard depth here (the DOM pipeline uses
                                  // kLess), not the scene's reversed-z.
                                  .clear = 1.0f};
        ctx.cmd->BeginRendering({.extent = res, .depth = &depth_att});
        ctx.cmd->BindPipeline(depth_pipeline_);
        record_grooms(ctx, false);
        ctx.cmd->EndRendering();
      });

  graph.AddPass(
      "hair_dom",
      [&](RenderGraph::PassBuilder& b) {
        b.Read(front, ResourceUsage::kSampledFragment);
        b.Write(layers, ResourceUsage::kColorAttachment);
      },
      [this, layers, res, record_grooms](PassContext& ctx) {
        ColorAttachment att{.view = ctx.graph->image(layers).view, .load = LoadOp::kClear};
        ctx.cmd->BeginRendering({.extent = res, .colors = {&att, 1}});
        ctx.cmd->BindPipeline(dom_pipeline_);
        record_grooms(ctx, true);
        ctx.cmd->EndRendering();
      });

  volume_valid_ = true;
}

void HairStrands::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                             Extent2D extent, const Frame& frame, u32 frame_slot) {
  if (!active()) return;

  // Publish this frame's simulated positions into the slot's buffer before
  // the graph executes (host-visible memory, no GPU copy).
  const u32 slot = frame_slot % kFramesInFlight;
  const u32 slot_bit = 1u << slot;
  for (Groom& g : grooms_) {
    if (!g.alive || !(g.stale & slot_bit)) continue;
    std::memcpy(g.points[slot].mapped, g.host_points.data(),
                g.host_points.size() * sizeof(HairPoint));
    g.stale &= ~slot_bit;
  }

  graph.AddPass(
      "hair_draw",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(color, ResourceUsage::kColorAttachment);
        b.Write(depth, ResourceUsage::kDepthAttachment);
        if (volume_valid_) {
          b.Read(volume_front_handle_, ResourceUsage::kSampledFragment);
          b.Read(volume_layers_handle_, ResourceUsage::kSampledFragment);
        }
      },
      [this, color, depth, extent, frame, slot](PassContext& ctx) {
        ColorAttachment att{.view = ctx.graph->image(color).view, .load = LoadOp::kLoad};
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = extent, .colors = {&att, 1}, .depth = &depth_att});
        ctx.cmd->BindPipeline(draw_pipeline_);
        Vec3 sun = Normalize(frame.sun_direction);
        for (Groom& g : grooms_) {
          if (!g.alive) continue;
          DrawPush push{};
          push.view_proj = frame.view_proj;
          push.camera[0] = frame.camera_pos.x;
          push.camera[1] = frame.camera_pos.y;
          push.camera[2] = frame.camera_pos.z;
          push.sun[0] = sun.x;
          push.sun[1] = sun.y;
          push.sun[2] = sun.z;
          push.sun[3] = frame.sun_intensity;
          push.sun_color[0] = frame.sun_color.x;
          push.sun_color[1] = frame.sun_color.y;
          push.sun_color[2] = frame.sun_color.z;
          FillGroomPush(push, g.strand_width, g.clump_radius, g.tint, g.children);
          ctx.cmd->BindTransient(
              0, {Bind::StorageBuffer(0, g.points[slot], 0, g.points[slot].size),
                  Bind::StorageBuffer(1, g.colors, 0, g.colors.size),
                  // The volume images are always bound; the shader gates on the
                  // caps instead. They are transitioned to a sampled state at
                  // creation and left in one by the graph, so a frame that
                  // skipped the volume passes still has something legal here.
                  Bind::Combined(2, front_depth_.view, volume_sampler_),
                  Bind::Combined(3, dom_.view, volume_sampler_),
                  Bind::Uniform(4, volume_params_[volume_slot_]),
                  Bind::Uniform(5, g.material)});
          ctx.cmd->Push(push);
          ctx.cmd->BindIndexBuffer(g.indices, 0, IndexType::kUint32);
          ctx.cmd->DrawIndexed(g.index_count, 1, 0, 0, 0);
        }
        ctx.cmd->EndRendering();
      });
}

}  // namespace rx::render
