#include "render/pipeline/virtual_geometry.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "asset/simplify.h"
#include "core/log.h"
#include "render/pipeline/meshlet.h"
#include "shaders/meshlet_ps_hlsl.h"
#include "shaders/vgeo_ms_hlsl.h"

namespace rx::render {
namespace {

struct VgeoPush {
  Mat4 view_proj;
  f32 planes[5][4];
  f32 camera[4];      // xyz eye, w proj_scale (screen px per world unit at d=1)
  f32 error_pixels;
  u32 meshlet_count;
  f32 pad0;
  f32 pad1;
};

struct GroupInfo {
  Vec3 center{};
  f32 radius = 0;
  f32 error = 0;
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

  pipeline_ = device.CreateGraphicsPipeline({
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
      .push_constant_size = sizeof(VgeoPush),
      .debug_name = "vgeo",
  });
  if (!pipeline_) {
    RX_ERROR("virtual geometry pipeline creation failed");
    return false;
  }
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    counters_[i] = device.CreateBuffer(16, kBufferUsageStorage, true);
  }
  return true;
}

void VirtualGeometryPass::Destroy(Device& device) {
  if (pipeline_) device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  for (GpuBuffer* b : {&meshlets_, &meshlet_vertices_, &meshlet_triangles_, &vertices_}) {
    if (*b) device.DestroyBuffer(*b);
    *b = {};
  }
  for (GpuBuffer& b : counters_) {
    if (b) device.DestroyBuffer(b);
    b = {};
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
  // locked borders stall the next simplification).
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

    // Morton-order the clusters so groups are spatial patches.
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

    std::vector<WorkCluster> next;
    bool progressed = false;
    for (u32 g = 0; g < count; g += kGroupSize) {
      u32 group_n = std::min(kGroupSize, count - g);
      std::vector<u32> group_indices;
      std::vector<u32> group_use(vertex_count, 0);
      f32 max_child_error = 0.0f;
      Vec3 center{};
      for (u32 k = 0; k < group_n; ++k) {
        const WorkCluster& c = current[order[g + k]];
        group_indices.insert(group_indices.end(), c.indices.begin(), c.indices.end());
        for (u32 v : c.indices) ++group_use[v];
        max_child_error = std::max(max_child_error, c.self_error);
        center = center + Vec3{c.self_sphere[0], c.self_sphere[1], c.self_sphere[2]};
      }
      center = center * (1.0f / static_cast<f32>(group_n));
      f32 radius = 0.0f;
      for (u32 k = 0; k < group_n; ++k) {
        const WorkCluster& c = current[order[g + k]];
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
      for (u32 k = 0; k < group_n; ++k) {
        WorkCluster& c = current[order[g + k]];
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
  RX_INFO("virtual geometry: {} clusters across {} levels ({} at lod0, {} tris in)",
           meshlet_count_, lod_count_, lod0, total_tris);
}

u32 VirtualGeometryPass::last_visible(u32 slot) const {
  const GpuBuffer& counter = counters_[slot % kFramesInFlight];
  return counter.mapped ? static_cast<const u32*>(counter.mapped)[0] : 0;
}

void VirtualGeometryPass::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                     ResourceHandle depth, const Mat4& view_proj,
                                     const f32 planes[5][4], const Vec3& camera, f32 proj_scale,
                                     f32 error_pixels, u32 slot) {
  if (!active()) return;
  VgeoPush push{};
  push.view_proj = view_proj;
  std::memcpy(push.planes, planes, sizeof(push.planes));
  push.camera[0] = camera.x;
  push.camera[1] = camera.y;
  push.camera[2] = camera.z;
  push.camera[3] = proj_scale;
  push.error_pixels = error_pixels;
  push.meshlet_count = meshlet_count_;

  graph.AddPass(
      "vgeo",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(color, ResourceUsage::kColorAttachment);
        b.Write(depth, ResourceUsage::kDepthAttachment);
      },
      [this, color, depth, push, slot](PassContext& ctx) {
        const GpuBuffer& counter = counters_[slot % kFramesInFlight];
        if (counter.mapped && slot % 120 == 60) {
          RX_INFO("vgeo: {} / {} clusters drawn (tau {} px)",
                   static_cast<const u32*>(counter.mapped)[0], meshlet_count_,
                   push.error_pixels);
        }
        if (counter.mapped) static_cast<u32*>(counter.mapped)[0] = 0;

        ColorAttachment att{.view = ctx.graph->image(color).view, .load = LoadOp::kLoad};
        DepthAttachment depth_att{.view = ctx.graph->image(depth).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = ctx.graph->image(color).extent,
                                 .colors = {&att, 1},
                                 .depth = &depth_att});
        ctx.cmd->BindPipeline(pipeline_);
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
