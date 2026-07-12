#include "render/geometry/hair_strands.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "shaders/hair_ps_hlsl.h"
#include "shaders/hair_vs_hlsl.h"

namespace rx::render {
namespace {

constexpr u32 kPointsPerStrand = kGroomPointsPerStrand;

struct DrawPush {
  Mat4 view_proj;
  f32 camera[4];     // xyz eye, w width
  f32 sun[4];        // xyz travel, w intensity
  f32 sun_color[4];  // rgb, w clump radius
  f32 tint[4];       // rgb tint, w children count
};

ByteSpan Span(const void* data, size_t bytes) {
  return ByteSpan(static_cast<const u8*>(data), bytes);
}

constexpr u32 kAllSlotsStale = (1u << HairStrands::kFramesInFlight) - 1;

}  // namespace

bool HairStrands::Initialize(Device& device, Format color_format, Format depth_format) {
  draw_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_hair_vs_hlsl),
      .fragment = RX_SHADER(k_hair_ps_hlsl),
      .raster = {.cull = CullMode::kNone},  // ribbons flip with the view
      .depth = {.test = true, .write = true, .compare = CompareOp::kGreaterEqual,
                .format = depth_format},
      .color_formats = {color_format},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer}, {1, BindingType::kStorageBuffer}},
                .stages = kShaderStageVertex}},
      .push_constant_size = sizeof(DrawPush),
      .debug_name = "hair_draw",
  });
  if (!draw_pipeline_) {
    RX_ERROR("hair pipeline creation failed");
    return false;
  }
  return true;
}

void HairStrands::Destroy(Device& device) {
  if (draw_pipeline_) device.DestroyPipeline(draw_pipeline_);
  draw_pipeline_ = {};
  for (Groom& g : grooms_) {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
      if (g.points[i]) device.DestroyBuffer(g.points[i]);
      g.points[i] = {};
    }
    for (GpuBuffer* b : {&g.colors, &g.indices}) {
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
  g.id = next_id_++;
  g.alive = true;
  grooms_.push_back(std::move(g));
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
  for (GpuBuffer* b : {&g->colors, &g->indices}) {
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
          push.camera[3] = g.strand_width;
          push.sun[0] = sun.x;
          push.sun[1] = sun.y;
          push.sun[2] = sun.z;
          push.sun[3] = frame.sun_intensity;
          push.sun_color[0] = frame.sun_color.x;
          push.sun_color[1] = frame.sun_color.y;
          push.sun_color[2] = frame.sun_color.z;
          push.sun_color[3] = g.clump_radius;
          push.tint[0] = g.tint.x;
          push.tint[1] = g.tint.y;
          push.tint[2] = g.tint.z;
          push.tint[3] = static_cast<f32>(g.children);
          ctx.cmd->BindTransient(
              0, {Bind::StorageBuffer(0, g.points[slot], 0, g.points[slot].size),
                  Bind::StorageBuffer(1, g.colors, 0, g.colors.size)});
          ctx.cmd->Push(push);
          ctx.cmd->BindIndexBuffer(g.indices, 0, IndexType::kUint32);
          ctx.cmd->DrawIndexed(g.index_count, 1, 0, 0, 0);
        }
        ctx.cmd->EndRendering();
      });
}

}  // namespace rx::render
