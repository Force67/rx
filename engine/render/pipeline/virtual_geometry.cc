#include "render/pipeline/virtual_geometry.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "asset/simplify.h"
#include "core/log.h"
#include "render/pipeline/meshlet.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/meshlet_ps_hlsl.h"
#include "shaders/vgeo_args_cs_hlsl.h"
#include "shaders/vgeo_clear_cs_hlsl.h"
#include "shaders/vgeo_cull_cs_hlsl.h"
#include "shaders/vgeo_hzb_cs_hlsl.h"
#include "shaders/vgeo_ms_hlsl.h"
#include "shaders/vgeo_resolve_ps_hlsl.h"
#include "shaders/vgeo_sw_cs_hlsl.h"
#include "shaders/vgeo_vis_ms_hlsl.h"
#include "shaders/vgeo_vis_ps_hlsl.h"

namespace rx::render {
namespace {

// Mirrors VgeoParams in vgeo_common.hlsli (std430).
struct Params {
  Mat4 view_proj;
  Mat4 prev_view_proj;
  f32 planes[5][4];
  f32 camera[4];       // xyz eye, w proj_scale
  f32 prev_camera[4];  // xyz last frame's eye, w tau
  f32 prev_hiz[4];     // prev hi-z w, h (0 = no occlusion), proj m00, m11
  f32 hiz[4];          // own hi-z w, h, unused
  u32 cluster_count;
  u32 instance_count;
  u32 width;
  u32 height;
  u32 sw_threshold;
  u32 debug;
  u32 max_visible;
  u32 pad;
};

// counters_/args_ slot indices; mirror the defines in vgeo_common.hlsli.
constexpr u32 kCounterVisible = 0;
constexpr u32 kCounterSw = 1;
constexpr u32 kCounterHw = 2;
constexpr u32 kCounterOccluded = 3;
constexpr u32 kArgsSwMain = 0;
constexpr u32 kArgsHwMain = 3;
constexpr u32 kArgsPostCull = 6;
constexpr u32 kArgsSwPost = 9;
constexpr u32 kArgsHwPost = 12;
constexpr u32 kArgsSlots = 15;

// Projected average triangle edge (px) below which a cluster rasters in
// compute; larger triangles keep fixed-function rasterization.
constexpr u32 kSwThresholdPx = 32;

// Legacy single-pass push block (vgeo.ms).
struct LegacyPush {
  Mat4 view_proj;
  f32 planes[5][4];
  f32 camera[4];  // xyz eye, w proj_scale
  f32 error_pixels;
  u32 meshlet_count;
  f32 pad0;
  f32 pad1;
};

u32 Morton3(u32 x, u32 y, u32 z) {
  auto part = [](u32 v) {
    v &= 0x3ff;
    v = (v | (v << 16)) & 0x030000ff;
    v = (v | (v << 8)) & 0x0300f00f;
    v = (v | (v << 4)) & 0x030c30c3;
    v = (v | (v << 2)) & 0x09249249;
    return v;
  };
  return part(x) | (part(y) << 1) | (part(z) << 2);
}

ByteSpan Span(const void* data, size_t bytes) {
  return ByteSpan(static_cast<const u8*>(data), bytes);
}

}  // namespace

bool VirtualGeometryPass::Initialize(Device& device, Format color_format, Format depth_format) {
  available_ = device.caps().mesh_shaders;
  if (!available_) return true;  // stays inert without mesh shaders
  gpu_driven_ = device.caps().buffer_atomics64;

  legacy_pipeline_ = device.CreateGraphicsPipeline({
      .fragment = RX_SHADER(k_meshlet_ps_hlsl),
      .mesh = RX_SHADER(k_vgeo_ms_hlsl),
      .raster = {.cull = CullMode::kBack},
      .depth = {.test = true, .write = true, .compare = CompareOp::kGreaterEqual,
                .format = depth_format},
      .color_formats = {color_format},
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kStorageBuffer},
                          {2, BindingType::kStorageBuffer},
                          {3, BindingType::kStorageBuffer},
                          {4, BindingType::kStorageBuffer}},
                .stages = kShaderStageMesh}},
      .push_constant_size = sizeof(LegacyPush),
      .debug_name = "vgeo_legacy",
  });
  if (!legacy_pipeline_) {
    RX_ERROR("virtual geometry legacy pipeline creation failed");
    return false;
  }
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    legacy_counters_[i] = device.CreateBuffer(16, kBufferUsageStorage, true);
  }
  if (!gpu_driven_) {
    RX_INFO("virtual geometry: no 64-bit buffer atomics, single-pass fallback");
    return true;
  }

  auto storage = [](u32 slot) { return BindingSlot{slot, BindingType::kStorageBuffer}; };

  cull_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_vgeo_cull_cs_hlsl),
      .sets = {{.slots = {storage(0), storage(1), storage(2), storage(3), storage(4),
                          storage(5), storage(6), storage(7),
                          {8, BindingType::kSampledImage}}}},
      .push_constant_size = sizeof(u32),
      .debug_name = "vgeo_cull",
  });
  args_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_vgeo_args_cs_hlsl),
      .sets = {{.slots = {storage(0), storage(1), storage(2)}}},
      .push_constant_size = sizeof(u32),
      .debug_name = "vgeo_args",
  });
  clear_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_vgeo_clear_cs_hlsl),
      .sets = {{.slots = {storage(0), storage(1)}}},
      .push_constant_size = sizeof(u32),
      .debug_name = "vgeo_clear",
  });
  sw_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_vgeo_sw_cs_hlsl),
      .sets = {{.slots = {storage(0), storage(1), storage(2), storage(3), storage(4),
                          storage(5), storage(6), storage(7), storage(8), storage(9)}}},
      .push_constant_size = sizeof(u32),
      .debug_name = "vgeo_sw",
  });
  hzb_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_vgeo_hzb_cs_hlsl),
      .sets = {{.slots = {storage(0),
                          {1, BindingType::kSampledImage},
                          storage(2),
                          {3, BindingType::kStorageImage}}}},
      .debug_name = "vgeo_hzb",
  });
  vis_pipeline_ = device.CreateGraphicsPipeline({
      .fragment = RX_SHADER(k_vgeo_vis_ps_hlsl),
      .mesh = RX_SHADER(k_vgeo_vis_ms_hlsl),
      .raster = {.cull = CullMode::kBack},
      .sets = {{.slots = {storage(0), storage(1), storage(2), storage(3), storage(4),
                          storage(5), storage(6), storage(7), storage(8), storage(9)}}},
      .push_constant_size = sizeof(u32),
      .debug_name = "vgeo_vis",
  });
  resolve_pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_fullscreen_vs_hlsl),
      .fragment = RX_SHADER(k_vgeo_resolve_ps_hlsl),
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true, .write = true, .compare = CompareOp::kGreaterEqual,
                .format = depth_format},
      .color_formats = {color_format},
      .sets = {{.slots = {storage(0), storage(1), storage(2), storage(3), storage(4),
                          storage(5), storage(6), storage(7)},
                .stages = kShaderStageFragment}},
      .debug_name = "vgeo_resolve",
  });
  if (!cull_pipeline_ || !args_pipeline_ || !clear_pipeline_ || !sw_pipeline_ ||
      !hzb_pipeline_ || !vis_pipeline_ || !resolve_pipeline_) {
    RX_ERROR("virtual geometry gpu-driven pipeline creation failed");
    return false;
  }

  visible_ = device.CreateBuffer(kMaxVisible * sizeof(u32) * 2, kBufferUsageStorage);
  sw_list_ = device.CreateBuffer(kMaxVisible * sizeof(u32), kBufferUsageStorage);
  hw_list_ = device.CreateBuffer(kMaxVisible * sizeof(u32), kBufferUsageStorage);
  occluded_ = device.CreateBuffer(kMaxVisible * sizeof(u32) * 2, kBufferUsageStorage);
  counters_ = device.CreateBuffer(kCounterSlots * sizeof(u32),
                                  kBufferUsageStorage | kBufferUsageTransferSrc);
  args_ = device.CreateBuffer(kArgsSlots * sizeof(u32),
                              kBufferUsageStorage | kBufferUsageIndirect);
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    params_[i] = device.CreateBuffer(sizeof(Params), kBufferUsageStorage, true);
    instances_[i] =
        device.CreateBuffer(kMaxInstances * sizeof(Mat4), kBufferUsageStorage, true);
    readback_[i] = device.CreateBuffer(kCounterSlots * sizeof(u32),
                                       kBufferUsageTransferDst, true);
  }

  // 1x1 fallback so the cull's hi-z descriptor is always valid; bound (with
  // occlusion disabled through the params) on frames without a real hi-z.
  dummy_hiz_ = device.CreateImage2D(Format::kR32Float, {1, 1}, kTextureUsageSampled);
  device.ImmediateSubmit([&](CommandList& cmd) {
    cmd.Barrier(Transition(dummy_hiz_, ResourceState::kUndefined,
                           ResourceState::kShaderReadAll));
  });
  return true;
}

void VirtualGeometryPass::Destroy(Device& device) {
  for (PipelineHandle* p : {&legacy_pipeline_, &cull_pipeline_, &args_pipeline_,
                            &clear_pipeline_, &sw_pipeline_, &hzb_pipeline_, &vis_pipeline_,
                            &resolve_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (GpuBuffer* b : {&meshlets_, &meshlet_vertices_, &meshlet_triangles_, &vertices_,
                       &visible_, &sw_list_, &hw_list_, &occluded_, &counters_, &args_,
                       &visbuffer_}) {
    if (*b) device.DestroyBuffer(*b);
    *b = {};
  }
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    for (GpuBuffer* b : {&params_[i], &instances_[i], &readback_[i], &legacy_counters_[i]}) {
      if (*b) device.DestroyBuffer(*b);
      *b = {};
    }
  }
  if (dummy_hiz_) device.DestroyImage(dummy_hiz_);
  dummy_hiz_ = {};
  for (GpuImage& img : hiz_img_) {
    if (img) device.DestroyImage(img);
    img = {};
  }
}

void VirtualGeometryPass::Upload(Device& device, const asset::Mesh& mesh) {
  if (!available_ || mesh.lods.empty()) return;
  const asset::MeshLod& lod = mesh.lods[0];
  const u32 vertex_count = static_cast<u32>(lod.vertices.size());

  std::vector<Vec3> positions(vertex_count);
  for (u32 i = 0; i < vertex_count; ++i) {
    positions[i] = {lod.vertices[i].position[0], lod.vertices[i].position[1],
                    lod.vertices[i].position[2]};
  }

  // A cluster of the level currently being processed: its triangles as global
  // indices plus the (sphere, error) pair of the group simplification that
  // created it. Level 0 clusters are exact (error 0, own bounds).
  struct WorkCluster {
    Meshlet m;                 // bounds/cone from the meshlet build
    std::vector<u32> indices;  // global triangle list
    f32 self_error = 0.0f;
    f32 self_sphere[4] = {0, 0, 0, 0};
    u32 dag_index = 0;         // where it landed in the output array
  };

  std::vector<DagMeshlet> dag;
  base::Vector<u32> all_vertex_indices;
  base::Vector<u32> all_triangles;

  // Emits one work cluster into the flat gpu arrays (re-derives the local
  // 8-bit indexing; inputs come from BuildMeshletGeometry so <=64 uniques).
  auto emit = [&](WorkCluster& c, u32 level) {
    DagMeshlet d{};
    std::memcpy(d.center_radius, c.m.center_radius, sizeof(d.center_radius));
    std::memcpy(d.cone, c.m.cone, sizeof(d.cone));
    std::memcpy(d.self_sphere, c.self_sphere, sizeof(d.self_sphere));
    std::memcpy(d.parent_sphere, c.self_sphere, sizeof(d.parent_sphere));
    d.self_error = c.self_error;
    d.parent_error = FLT_MAX;
    d.lod = level;
    d.vertex_offset = static_cast<u32>(all_vertex_indices.size());
    d.triangle_offset = static_cast<u32>(all_triangles.size());
    u32 local_map[64];
    u32 local_count = 0;
    for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
      u32 local[3];
      for (u32 k = 0; k < 3; ++k) {
        u32 g = c.indices[t + k];
        u32 found = 0xffffffffu;
        for (u32 l = 0; l < local_count; ++l) {
          if (local_map[l] == g) {
            found = l;
            break;
          }
        }
        if (found == 0xffffffffu) {
          found = local_count;
          local_map[local_count++] = g;
          all_vertex_indices.push_back(g);
        }
        local[k] = found;
      }
      all_triangles.push_back(local[0] | (local[1] << 8) | (local[2] << 16));
    }
    d.vertex_count = local_count;
    d.triangle_count = static_cast<u32>(c.indices.size() / 3);
    c.dag_index = static_cast<u32>(dag.size());
    dag.push_back(d);
  };

  // Turns an index list into work clusters via the meshlet builder (cone
  // splitting off: coarse levels would fragment into tiny clusters whose
  // locked borders stall further simplification).
  auto make_clusters = [&](const std::vector<u32>& indices, f32 self_error,
                           const f32 self_sphere[4], std::vector<WorkCluster>* out) {
    MeshletGeometry geo =
        BuildMeshletGeometry(lod.vertices.data(), vertex_count, indices.data(),
                             static_cast<u32>(indices.size()), /*cone_split=*/false);
    for (const Meshlet& m : geo.meshlets) {
      WorkCluster c;
      c.m = m;
      c.self_error = self_error;
      if (self_sphere) {
        std::memcpy(c.self_sphere, self_sphere, sizeof(c.self_sphere));
      } else {
        std::memcpy(c.self_sphere, m.center_radius, sizeof(c.self_sphere));
      }
      c.indices.reserve(static_cast<size_t>(m.triangle_count) * 3);
      for (u32 t = 0; t < m.triangle_count; ++t) {
        u32 packed = geo.triangles[m.triangle_offset + t];
        for (u32 k = 0; k < 3; ++k) {
          c.indices.push_back(geo.vertex_indices[m.vertex_offset + ((packed >> (8 * k)) & 0xffu)]);
        }
      }
      out->push_back(std::move(c));
    }
  };

  std::vector<WorkCluster> current;
  {
    std::vector<u32> root_indices(lod.indices.begin(), lod.indices.end());
    make_clusters(root_indices, 0.0f, nullptr, &current);
  }

  constexpr u32 kMaxLevels = 16;
  constexpr u32 kGroupSize = 8;
  u32 level = 0;
  u32 total_tris = static_cast<u32>(lod.indices.size() / 3);
  while (true) {
    for (WorkCluster& c : current) emit(c, level);
    if (current.size() <= 2 || level + 1 >= kMaxLevels) break;

    // Morton-order the clusters so group seeds walk spatial patches.
    const u32 count = static_cast<u32>(current.size());
    std::vector<u32> order(count);
    for (u32 i = 0; i < count; ++i) order[i] = i;
    Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (const WorkCluster& c : current) {
      lo = {std::min(lo.x, c.m.center_radius[0]), std::min(lo.y, c.m.center_radius[1]),
            std::min(lo.z, c.m.center_radius[2])};
      hi = {std::max(hi.x, c.m.center_radius[0]), std::max(hi.y, c.m.center_radius[1]),
            std::max(hi.z, c.m.center_radius[2])};
    }
    Vec3 ext{std::max(hi.x - lo.x, 1e-6f), std::max(hi.y - lo.y, 1e-6f),
             std::max(hi.z - lo.z, 1e-6f)};
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
      auto code = [&](u32 i) {
        const f32* c = current[i].m.center_radius;
        return Morton3(static_cast<u32>((c[0] - lo.x) / ext.x * 1023.0f),
                       static_cast<u32>((c[1] - lo.y) / ext.y * 1023.0f),
                       static_cast<u32>((c[2] - lo.z) / ext.z * 1023.0f));
      };
      return code(a) < code(b);
    });

    // Vertex use counts across the level: a vertex referenced outside the
    // group is a border and must be locked.
    std::vector<u32> use_count(vertex_count, 0);
    for (const WorkCluster& c : current) {
      for (u32 v : c.indices) ++use_count[v];
    }

    // Cluster adjacency through shared vertices: groups grow along mesh
    // connectivity instead of raw morton runs, so fewer vertices sit on group
    // borders and the locked-edge simplification keeps making progress on the
    // coarse levels (morton runs used to strand groups that refuse to shrink).
    std::unordered_map<u32, std::vector<u32>> vertex_clusters;
    for (u32 i = 0; i < count; ++i) {
      for (u32 v : current[i].indices) {
        auto& list = vertex_clusters[v];
        if (list.empty() || list.back() != i) list.push_back(i);
      }
    }

    std::vector<u8> grouped(count, 0);
    std::vector<u32> score(count, 0);
    std::vector<u32> touched;
    std::vector<WorkCluster> next;
    bool progressed = false;
    for (u32 seed_pos = 0; seed_pos < count; ++seed_pos) {
      u32 seed = order[seed_pos];
      if (grouped[seed]) continue;

      // Greedy fill: repeatedly take the ungrouped neighbor sharing the most
      // vertices with the group so far.
      std::vector<u32> members;
      touched.clear();
      auto add_member = [&](u32 ci) {
        members.push_back(ci);
        grouped[ci] = 1;
        for (u32 v : current[ci].indices) {
          for (u32 nb : vertex_clusters[v]) {
            if (grouped[nb]) continue;
            if (score[nb] == 0) touched.push_back(nb);
            ++score[nb];
          }
        }
      };
      add_member(seed);
      while (members.size() < kGroupSize) {
        u32 best = 0xffffffffu;
        u32 best_score = 0;
        for (u32 nb : touched) {
          if (!grouped[nb] && score[nb] > best_score) {
            best = nb;
            best_score = score[nb];
          }
        }
        if (best == 0xffffffffu) break;
        add_member(best);
      }
      for (u32 nb : touched) score[nb] = 0;

      std::vector<u32> group_indices;
      std::vector<u32> group_use(vertex_count, 0);
      f32 max_child_error = 0.0f;
      Vec3 center{};
      for (u32 ci : members) {
        const WorkCluster& c = current[ci];
        group_indices.insert(group_indices.end(), c.indices.begin(), c.indices.end());
        for (u32 v : c.indices) ++group_use[v];
        max_child_error = std::max(max_child_error, c.self_error);
        center = center + Vec3{c.self_sphere[0], c.self_sphere[1], c.self_sphere[2]};
      }
      center = center * (1.0f / static_cast<f32>(members.size()));
      f32 radius = 0.0f;
      for (u32 ci : members) {
        const WorkCluster& c = current[ci];
        Vec3 d = Vec3{c.self_sphere[0], c.self_sphere[1], c.self_sphere[2]} - center;
        radius = std::max(radius,
                          std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) + c.self_sphere[3]);
      }
      f32 group_sphere[4] = {center.x, center.y, center.z, radius};

      std::vector<u8> lock(vertex_count, 0);
      for (u32 idx : group_indices) {
        if (group_use[idx] < use_count[idx]) lock[idx] = 1;
      }

      f32 err = 0.0f;
      std::vector<u32> simplified = asset::SimplifyIndices(
          positions.data(), vertex_count, group_indices.data(),
          static_cast<u32>(group_indices.size()),
          static_cast<u32>(group_indices.size()) / 2, lock.data(), &err);
      // A group that refuses to shrink stays terminal: its clusters keep
      // FLT_MAX parents and simply draw whenever their own error passes.
      if (simplified.size() >= group_indices.size() * 4 / 5) continue;
      progressed = true;

      // Monotonic error up the DAG.
      f32 group_error = std::max(err, max_child_error);
      for (u32 ci : members) {
        WorkCluster& c = current[ci];
        DagMeshlet& d = dag[c.dag_index];
        d.parent_error = group_error;
        std::memcpy(d.parent_sphere, group_sphere, sizeof(d.parent_sphere));
      }
      // The group's replacement clusters inherit the exact same sphere+error
      // pair their children compare against: the cut cannot leave gaps.
      make_clusters(simplified, group_error, group_sphere, &next);
    }
    if (!progressed) break;
    u32 next_tris = 0;
    for (const WorkCluster& c : next) next_tris += static_cast<u32>(c.indices.size() / 3);
    RX_INFO("vgeo dag: level {} {} clusters -> level {} {} clusters ({} tris)", level, count,
             level + 1, next.size(), next_tris);
    current = std::move(next);
    ++level;
  }

  lod_count_ = level + 1;
  meshlet_count_ = static_cast<u32>(dag.size());
  if (meshlet_count_ == 0) return;

  std::vector<MeshletPass::Vertex> verts;
  verts.reserve(lod.vertices.size());
  for (const asset::Vertex& v : lod.vertices) {
    verts.push_back({v.position[0], v.position[1], v.position[2], v.normal[0], v.normal[1],
                     v.normal[2]});
  }

  for (GpuBuffer* b : {&meshlets_, &meshlet_vertices_, &meshlet_triangles_, &vertices_}) {
    if (*b) device.DestroyBuffer(*b);
  }
  const BufferUsageFlags storage = kBufferUsageStorage;
  meshlets_ =
      device.CreateBufferWithData(Span(dag.data(), dag.size() * sizeof(DagMeshlet)), storage);
  meshlet_vertices_ = device.CreateBufferWithData(
      Span(all_vertex_indices.data(), all_vertex_indices.size() * sizeof(u32)), storage);
  meshlet_triangles_ = device.CreateBufferWithData(
      Span(all_triangles.data(), all_triangles.size() * sizeof(u32)), storage);
  vertices_ = device.CreateBufferWithData(
      Span(verts.data(), verts.size() * sizeof(MeshletPass::Vertex)), storage);

  u32 lod0 = 0;
  for (const DagMeshlet& d : dag) lod0 += d.lod == 0 ? 1 : 0;
  RX_INFO("virtual geometry: {} clusters across {} levels ({} at lod0, {} tris in, {})",
           meshlet_count_, lod_count_, lod0, total_tris,
           gpu_driven_ ? "gpu-driven" : "single-pass");
}

void VirtualGeometryPass::SetInstances(std::span<const Mat4> transforms) {
  pending_instances_.clear();
  for (const Mat4& m : transforms) {
    if (pending_instances_.size() >= kMaxInstances) break;
    pending_instances_.push_back(m);
  }
}

VirtualGeometryPass::Stats VirtualGeometryPass::last_stats(u32 slot) const {
  if (!gpu_driven_) {
    const GpuBuffer& counter = legacy_counters_[slot % kFramesInFlight];
    Stats s;
    s.visible = counter.mapped ? static_cast<const u32*>(counter.mapped)[0] : 0;
    s.hw = s.visible;
    return s;
  }
  const GpuBuffer& rb = readback_[slot % kFramesInFlight];
  if (!rb.mapped) return {};
  const u32* c = static_cast<const u32*>(rb.mapped);
  Stats s;
  s.visible = c[kCounterVisible];
  s.sw = c[kCounterSw];
  s.hw = c[kCounterHw];
  s.occluded = c[kCounterOccluded];
  return s;
}

void VirtualGeometryPass::EnsureTargets(Device& device, u32 width, u32 height) {
  const u32 hiz_w = (width + kHizDownsample - 1) / kHizDownsample;
  const u32 hiz_h = (height + kHizDownsample - 1) / kHizDownsample;
  if (!hiz_img_[0] || hiz_img_[0].extent.width != hiz_w ||
      hiz_img_[0].extent.height != hiz_h) {
    for (u32 i = 0; i < kFramesInFlight; ++i) {
      if (hiz_img_[i]) device.DestroyImage(hiz_img_[i]);
      hiz_img_[i] = device.CreateImage2D(Format::kR32Float, {hiz_w, hiz_h},
                                         kTextureUsageStorage | kTextureUsageSampled);
      hiz_state_[i] = ResourceState::kUndefined;
      hiz_dims_[i][0] = 0;  // stale until this slot's map is rebuilt
      hiz_dims_[i][1] = 0;
    }
  }
  if (visbuffer_ && u64(width) * height * sizeof(u64) <= visbuffer_.size) {
    vis_width_ = width;
    vis_height_ = height;
    return;
  }
  // Grow-only; genuine resizes go through the renderer's wait-idle path.
  if (visbuffer_) device.DestroyBuffer(visbuffer_);
  visbuffer_ = device.CreateBuffer(u64(width) * height * sizeof(u64), kBufferUsageStorage);
  vis_width_ = width;
  vis_height_ = height;
}

void VirtualGeometryPass::AddToGraph(Device& device, RenderGraph& graph, const Frame& frame) {
  if (!active()) return;
  if (!gpu_driven_) {
    AddLegacyPass(graph, frame);
    return;
  }
  if (frame.width == 0 || frame.height == 0) return;
  EnsureTargets(device, frame.width, frame.height);

  const u32 slot = frame.slot % kFramesInFlight;
  const u32 prev_slot = slot ^ 1u;
  const u32 hiz_w = (frame.width + kHizDownsample - 1) / kHizDownsample;
  const u32 hiz_h = (frame.height + kHizDownsample - 1) / kHizDownsample;
  const bool has_occlusion =
      has_prev_ && hiz_dims_[prev_slot][0] == hiz_w && hiz_dims_[prev_slot][1] == hiz_h;

  // Instance transforms for this frame.
  if (instances_[slot].mapped) {
    Mat4* dst = static_cast<Mat4*>(instances_[slot].mapped);
    if (pending_instances_.empty()) {
      dst[0] = Mat4::Identity();
      instance_count_ = 1;
    } else {
      std::memcpy(dst, pending_instances_.data(), pending_instances_.size() * sizeof(Mat4));
      instance_count_ = static_cast<u32>(pending_instances_.size());
    }
  }

  if (params_[slot].mapped) {
    Params p{};
    p.view_proj = frame.view_proj;
    p.prev_view_proj = has_prev_ ? prev_view_proj_ : frame.view_proj;
    std::memcpy(p.planes, frame.planes, sizeof(p.planes));
    p.camera[0] = frame.eye.x;
    p.camera[1] = frame.eye.y;
    p.camera[2] = frame.eye.z;
    p.camera[3] = frame.proj_scale;
    const Vec3 prev_eye = has_prev_ ? prev_eye_ : frame.eye;
    p.prev_camera[0] = prev_eye.x;
    p.prev_camera[1] = prev_eye.y;
    p.prev_camera[2] = prev_eye.z;
    p.prev_camera[3] = frame.error_pixels;
    p.prev_hiz[0] = has_occlusion ? static_cast<f32>(hiz_w) : 0.0f;
    p.prev_hiz[1] = has_occlusion ? static_cast<f32>(hiz_h) : 0.0f;
    p.prev_hiz[2] = frame.proj_m00;
    p.prev_hiz[3] = frame.proj_m11;
    p.hiz[0] = static_cast<f32>(hiz_w);
    p.hiz[1] = static_cast<f32>(hiz_h);
    p.hiz[2] = frame.near_plane;
    p.cluster_count = meshlet_count_;
    p.instance_count = instance_count_;
    p.width = frame.width;
    p.height = frame.height;
    p.sw_threshold = kSwThresholdPx;
    if (const char* env = std::getenv("RX_VGEO_SW_EDGE")) {
      p.sw_threshold = static_cast<u32>(std::max(std::atoi(env), 0));
    }
    p.debug = frame.debug;
    p.max_visible = kMaxVisible;
    std::memcpy(params_[slot].mapped, &p, sizeof(p));
  }

  if (frame.debug != 0 && frame.slot % 60 == 30) {
    Stats s = last_stats(frame.slot);
    RX_INFO("vgeo: {} visible ({} sw / {} hw), {} deferred, {} clusters x {} instances",
             s.visible, s.sw, s.hw, s.occluded, meshlet_count_, instance_count_);
  }

  ResourceHandle hzb =
      graph.ImportImage("vgeo_hiz", hiz_img_[slot], &hiz_state_[slot]);
  ResourceHandle prev_hzb =
      has_occlusion ? graph.ImportImage("vgeo_prev_hiz", hiz_img_[prev_slot],
                                        &hiz_state_[prev_slot])
                    : kInvalidResource;

  auto bind_cull = [this, slot](PassContext& ctx, TextureView hiz_view) {
    ctx.cmd->BindPipeline(cull_pipeline_);
    ctx.cmd->BindTransient(
        0, {Bind::StorageBuffer(0, params_[slot]), Bind::StorageBuffer(1, meshlets_),
            Bind::StorageBuffer(2, instances_[slot]), Bind::StorageBuffer(3, visible_),
            Bind::StorageBuffer(4, sw_list_), Bind::StorageBuffer(5, hw_list_),
            Bind::StorageBuffer(6, occluded_), Bind::StorageBuffer(7, counters_),
            Bind::SampledView(8, hiz_view)});
  };
  auto bind_sw = [this, slot](PassContext& ctx) {
    ctx.cmd->BindPipeline(sw_pipeline_);
    ctx.cmd->BindTransient(
        0, {Bind::StorageBuffer(0, params_[slot]), Bind::StorageBuffer(1, meshlets_),
            Bind::StorageBuffer(2, meshlet_vertices_),
            Bind::StorageBuffer(3, meshlet_triangles_), Bind::StorageBuffer(4, vertices_),
            Bind::StorageBuffer(5, instances_[slot]), Bind::StorageBuffer(6, visible_),
            Bind::StorageBuffer(7, sw_list_), Bind::StorageBuffer(8, visbuffer_),
            Bind::StorageBuffer(9, counters_)});
  };
  auto bind_args = [this, slot](PassContext& ctx) {
    ctx.cmd->BindPipeline(args_pipeline_);
    ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, params_[slot]),
                               Bind::StorageBuffer(1, counters_),
                               Bind::StorageBuffer(2, args_)});
  };
  const u32 width = frame.width;
  const u32 height = frame.height;

  // 1: visibility buffer + counters to zero.
  graph.AddPass(
      "vgeo_clear", [](RenderGraph::PassBuilder&) {},
      [this, width, height](PassContext& ctx) {
        ctx.cmd->BindPipeline(clear_pipeline_);
        ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, visbuffer_),
                                   Bind::StorageBuffer(1, counters_)});
        u32 pixels = width * height;
        ctx.cmd->Push(pixels);
        ctx.cmd->Dispatch((pixels + 63) / 64, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kAllCommands);
      });

  // 2: main cull - DAG cut, frustum, cone, then the previous frame's hi-z.
  graph.AddPass(
      "vgeo_cull_main",
      [&](RenderGraph::PassBuilder& b) {
        if (has_occlusion) b.Read(prev_hzb, ResourceUsage::kSampledCompute);
      },
      [this, bind_cull, prev_hzb, has_occlusion](PassContext& ctx) {
        TextureView hiz_view =
            has_occlusion ? ctx.graph->image(prev_hzb).view : dummy_hiz_.view;
        bind_cull(ctx, hiz_view);
        ctx.cmd->Push(0u);  // mode 0
        u32 total = meshlet_count_ * instance_count_;
        ctx.cmd->Dispatch((total + 63) / 64, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kAllCommands);
      });

  // 3: fold counts into indirect args.
  graph.AddPass(
      "vgeo_args_main", [](RenderGraph::PassBuilder&) {},
      [bind_args](PassContext& ctx) {
        bind_args(ctx);
        ctx.cmd->Push(0u);
        ctx.cmd->Dispatch(1, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kAllCommands);
      });

  // 4: main raster - compute for the small clusters, mesh shader for the rest.
  graph.AddPass(
      "vgeo_sw_main", [](RenderGraph::PassBuilder&) {},
      [bind_sw, this](PassContext& ctx) {
        bind_sw(ctx);
        ctx.cmd->Push(0u);
        ctx.cmd->DispatchIndirect(args_, kArgsSwMain * sizeof(u32));
      });
  auto hw_raster = [this, slot, width, height](PassContext& ctx, u32 mode, u32 args_offset) {
    ctx.cmd->BeginRendering({.extent = {width, height}});
    ctx.cmd->BindPipeline(vis_pipeline_);
    ctx.cmd->BindTransient(
        0, {Bind::StorageBuffer(0, params_[slot]), Bind::StorageBuffer(1, meshlets_),
            Bind::StorageBuffer(2, meshlet_vertices_),
            Bind::StorageBuffer(3, meshlet_triangles_), Bind::StorageBuffer(4, vertices_),
            Bind::StorageBuffer(5, instances_[slot]), Bind::StorageBuffer(6, visible_),
            Bind::StorageBuffer(7, hw_list_), Bind::StorageBuffer(8, visbuffer_),
            Bind::StorageBuffer(9, counters_)});
    ctx.cmd->Push(mode);
    ctx.cmd->DrawMeshTasksIndirect(args_, args_offset * sizeof(u32), 1, 3 * sizeof(u32));
    ctx.cmd->EndRendering();
  };
  graph.AddPass(
      "vgeo_hw_main", [](RenderGraph::PassBuilder&) {},
      [hw_raster](PassContext& ctx) { hw_raster(ctx, 0u, kArgsHwMain); });

  // 5: rebuild the hi-z from this frame's occluders (scene depth + step 4).
  // Runs again after the post raster so the map handed to next frame's main
  // cull covers the disoccluded clusters too.
  auto add_hzb_pass = [&graph, this, slot, hzb, hiz_w, hiz_h,
                       depth = frame.depth](const char* name) {
    graph.AddPass(
        name,
        [&](RenderGraph::PassBuilder& b) {
          b.Read(depth, ResourceUsage::kSampledCompute);
          b.Write(hzb, ResourceUsage::kStorageWrite);
        },
        [this, slot, hzb, hiz_w, hiz_h, depth](PassContext& ctx) {
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
          ctx.cmd->MemoryBarrier(BarrierScope::kGraphicsStorageWrite,
                                 BarrierScope::kComputeRead);
          ctx.cmd->BindPipeline(hzb_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::StorageBuffer(0, params_[slot]),
                                     Bind::Sampled(1, ctx.graph->image(depth)),
                                     Bind::StorageBuffer(2, visbuffer_),
                                     Bind::Storage(3, ctx.graph->image(hzb))});
          ctx.cmd->Dispatch2D({hiz_w, hiz_h});
        });
  };
  add_hzb_pass("vgeo_hzb");

  // 6: retest the deferred clusters against the fresh hi-z, then their raster.
  graph.AddPass(
      "vgeo_cull_post",
      [&](RenderGraph::PassBuilder& b) { b.Read(hzb, ResourceUsage::kSampledCompute); },
      [this, bind_cull, hzb](PassContext& ctx) {
        bind_cull(ctx, ctx.graph->image(hzb).view);
        ctx.cmd->Push(1u);  // mode 1
        ctx.cmd->DispatchIndirect(args_, kArgsPostCull * sizeof(u32));
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kAllCommands);
      });
  graph.AddPass(
      "vgeo_args_post", [](RenderGraph::PassBuilder&) {},
      [bind_args](PassContext& ctx) {
        bind_args(ctx);
        ctx.cmd->Push(1u);
        ctx.cmd->Dispatch(1, 1, 1);
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kAllCommands);
      });
  graph.AddPass(
      "vgeo_sw_post", [](RenderGraph::PassBuilder&) {},
      [bind_sw, this](PassContext& ctx) {
        bind_sw(ctx);
        ctx.cmd->Push(1u);
        ctx.cmd->DispatchIndirect(args_, kArgsSwPost * sizeof(u32));
      });
  graph.AddPass(
      "vgeo_hw_post", [](RenderGraph::PassBuilder&) {},
      [hw_raster](PassContext& ctx) { hw_raster(ctx, 1u, kArgsHwPost); });
  add_hzb_pass("vgeo_hzb_post");

  // 7: fullscreen resolve into the lit target + scene depth.
  graph.AddPass(
      "vgeo_resolve",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(frame.color, ResourceUsage::kColorAttachment);
        b.Write(frame.depth, ResourceUsage::kDepthAttachment);
      },
      [this, slot, color = frame.color, depth = frame.depth](PassContext& ctx) {
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        ctx.cmd->MemoryBarrier(BarrierScope::kGraphicsStorageWrite,
                               BarrierScope::kGraphicsRead);
        ColorAttachment att{.view = ctx.graph->image(color).view, .load = LoadOp::kLoad};
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view,
                                  .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = ctx.graph->image(color).extent,
                                 .colors = {&att, 1},
                                 .depth = &depth_att});
        ctx.cmd->BindPipeline(resolve_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::StorageBuffer(0, params_[slot]), Bind::StorageBuffer(1, meshlets_),
                Bind::StorageBuffer(2, meshlet_vertices_),
                Bind::StorageBuffer(3, meshlet_triangles_), Bind::StorageBuffer(4, vertices_),
                Bind::StorageBuffer(5, instances_[slot]), Bind::StorageBuffer(6, visible_),
                Bind::StorageBuffer(7, visbuffer_)});
        ctx.cmd->Draw(3, 1, 0, 0);
        ctx.cmd->EndRendering();
      });

  // 8: counters to the host for the debug overlay (read two frames later).
  graph.AddPass(
      "vgeo_stats", [](RenderGraph::PassBuilder&) {},
      [this, slot](PassContext& ctx) {
        ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kTransferRead);
        ctx.cmd->CopyBuffer(counters_, 0, readback_[slot], 0, kCounterSlots * sizeof(u32));
      });

  hiz_dims_[slot][0] = hiz_w;
  hiz_dims_[slot][1] = hiz_h;
  prev_view_proj_ = frame.view_proj;
  prev_eye_ = frame.eye;
  has_prev_ = true;
}

void VirtualGeometryPass::AddLegacyPass(RenderGraph& graph, const Frame& frame) {
  LegacyPush push{};
  push.view_proj = frame.view_proj;
  std::memcpy(push.planes, frame.planes, sizeof(push.planes));
  push.camera[0] = frame.eye.x;
  push.camera[1] = frame.eye.y;
  push.camera[2] = frame.eye.z;
  push.camera[3] = frame.proj_scale;
  push.error_pixels = frame.error_pixels;
  push.meshlet_count = meshlet_count_;

  graph.AddPass(
      "vgeo",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(frame.color, ResourceUsage::kColorAttachment);
        b.Write(frame.depth, ResourceUsage::kDepthAttachment);
      },
      [this, color = frame.color, depth = frame.depth, push, slot = frame.slot](
          PassContext& ctx) {
        const GpuBuffer& counter = legacy_counters_[slot % kFramesInFlight];
        if (counter.mapped) static_cast<u32*>(counter.mapped)[0] = 0;

        ColorAttachment att{.view = ctx.graph->image(color).view, .load = LoadOp::kLoad};
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view,
                                  .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = ctx.graph->image(color).extent,
                                 .colors = {&att, 1},
                                 .depth = &depth_att});
        ctx.cmd->BindPipeline(legacy_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::StorageBuffer(0, meshlets_, 0, meshlets_.size),
                Bind::StorageBuffer(1, meshlet_vertices_, 0, meshlet_vertices_.size),
                Bind::StorageBuffer(2, meshlet_triangles_, 0, meshlet_triangles_.size),
                Bind::StorageBuffer(3, vertices_, 0, vertices_.size),
                Bind::StorageBuffer(4, counter, 0, counter.size)});
        ctx.cmd->Push(push);
        ctx.cmd->DrawMeshTasks(meshlet_count_, 1, 1);
        ctx.cmd->EndRendering();
      });
}

}  // namespace rx::render
