#include "render/gi/sdf_scene.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/math.h"

namespace rx::render {
namespace {

struct Tri {
  Vec3 a, b, c;
};

// Closest point on triangle abc to p (Ericson, Real-Time Collision Detection).
Vec3 ClosestPointTriangle(const Vec3& p, const Tri& t) {
  Vec3 ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
  f32 d1 = Dot(ab, ap), d2 = Dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return t.a;
  Vec3 bp = p - t.b;
  f32 d3 = Dot(ab, bp), d4 = Dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return t.b;
  f32 vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return t.a + ab * (d1 / (d1 - d3));
  Vec3 cp = p - t.c;
  f32 d5 = Dot(ab, cp), d6 = Dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return t.c;
  f32 vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return t.a + ac * (d2 / (d2 - d6));
  f32 va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
    return t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
  f32 denom = 1.0f / (va + vb + vc);
  f32 v = vb * denom, w = vc * denom;
  return t.a + ab * v + ac * w;
}

f32 PointTriDistanceSq(const Vec3& p, const Tri& t) {
  Vec3 q = ClosestPointTriangle(p, t);
  Vec3 d = p - q;
  return Dot(d, d);
}

// Ray p + axis_dir*s (s > 0), axis in {0,1,2}. Returns true and the along-axis
// distance s when the ray crosses the triangle. Möller-Trumbore specialised to
// an axis-aligned direction.
bool AxisRayCross(const Vec3& p, u32 axis, const Tri& t, f32& s) {
  Vec3 dir{axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f, axis == 2 ? 1.0f : 0.0f};
  Vec3 e1 = t.b - t.a, e2 = t.c - t.a;
  Vec3 pv = Cross(dir, e2);
  f32 det = Dot(e1, pv);
  if (std::fabs(det) < 1e-12f) return false;
  f32 inv = 1.0f / det;
  Vec3 tv = p - t.a;
  f32 u = Dot(tv, pv) * inv;
  if (u < 0 || u > 1) return false;
  Vec3 qv = Cross(tv, e1);
  f32 v = Dot(dir, qv) * inv;
  if (v < 0 || u + v > 1) return false;
  s = Dot(e2, qv) * inv;
  return s > 1e-6f;
}

// Uniform triangle grid over the volume box, CSR triangle lists per cell.
struct Grid {
  Vec3 origin;  // box min corner
  f32 cell = 1;
  int n[3] = {1, 1, 1};
  base::Vector<u32> start;    // n[0]*n[1]*n[2] + 1
  base::Vector<u32> tri_ids;  // triangle index per (cell,tri) entry

  int Index(int x, int y, int z) const { return (z * n[1] + y) * n[0] + x; }
  int ClampCell(f32 v, int axis) const {
    int c = static_cast<int>(std::floor((v - (&origin.x)[axis]) / cell));
    return std::clamp(c, 0, n[axis] - 1);
  }
};

void BuildGrid(Grid& g, const base::Vector<Tri>& tris, const Vec3& box_min, const Vec3& box_ext,
               f32 cell) {
  g.origin = box_min;
  g.cell = cell;
  for (int a = 0; a < 3; ++a)
    g.n[a] = std::max(1, static_cast<int>(std::ceil((&box_ext.x)[a] / cell)));
  const int cells = g.n[0] * g.n[1] * g.n[2];
  base::Vector<u32> counts;
  counts.resize(static_cast<size_t>(cells) + 1, 0u);
  auto tri_cell_range = [&](const Tri& t, int lo[3], int hi[3]) {
    for (int a = 0; a < 3; ++a) {
      f32 mn = std::min({(&t.a.x)[a], (&t.b.x)[a], (&t.c.x)[a]});
      f32 mx = std::max({(&t.a.x)[a], (&t.b.x)[a], (&t.c.x)[a]});
      lo[a] = std::clamp(static_cast<int>(std::floor((mn - (&box_min.x)[a]) / cell)), 0, g.n[a] - 1);
      hi[a] = std::clamp(static_cast<int>(std::floor((mx - (&box_min.x)[a]) / cell)), 0, g.n[a] - 1);
    }
  };
  for (const Tri& t : tris) {
    int lo[3], hi[3];
    tri_cell_range(t, lo, hi);
    for (int z = lo[2]; z <= hi[2]; ++z)
      for (int y = lo[1]; y <= hi[1]; ++y)
        for (int x = lo[0]; x <= hi[0]; ++x) counts[g.Index(x, y, z) + 1]++;
  }
  for (int i = 0; i < cells; ++i) counts[i + 1] += counts[i];
  g.start = counts;  // prefix-summed offsets
  g.tri_ids.resize(counts[cells], 0u);
  base::Vector<u32> cursor = g.start;
  for (u32 ti = 0; ti < tris.size(); ++ti) {
    int lo[3], hi[3];
    tri_cell_range(tris[ti], lo, hi);
    for (int z = lo[2]; z <= hi[2]; ++z)
      for (int y = lo[1]; y <= hi[1]; ++y)
        for (int x = lo[0]; x <= hi[0]; ++x) g.tri_ids[cursor[g.Index(x, y, z)]++] = ti;
  }
}

}  // namespace

SdfScene::~SdfScene() {
  jobs_.WaitIdle();
  for (auto entry : meshes_) {
    if (entry.value.sdf) device_.DestroyBuffer(entry.value.sdf);
  }
}

void SdfScene::Remove(u64 mesh_key) {
  MeshSdf* existing = meshes_.find(mesh_key);
  if (!existing) return;
  // Frame-safe: an in-flight compose may still read the buffer, so retire it
  // through the per-frame graveyard instead of stalling the whole device. The
  // map entry goes away now, so no new frame can bind it.
  if (existing->sdf) {
    total_bytes_ -= existing->sdf.size;
    device_.DestroyBufferDeferred(existing->sdf);
  }
  meshes_.erase(mesh_key);
}

bool SdfScene::RegisterMesh(u64 mesh_key, const MeshInput& input) {
  // Re-uploading under an existing key replaces the geometry (Renderer::UploadMesh
  // destroys and rebuilds the GPU buffers), so the old SDF is stale and must be
  // regenerated -- keeping the first one (an early return) would permanently pin
  // a bind-pose / previous mesh. Retire the previous buffer through the deferred
  // graveyard (an in-flight compose may still read it; no device stall needed),
  // then fall through and rebuild below.
  if (MeshSdf* existing = meshes_.find(mesh_key)) {
    if (input.vertex_count == 0) return true;  // nothing to replace it with; keep the current SDF
    if (existing->sdf) {
      total_bytes_ -= existing->sdf.size;
      device_.DestroyBufferDeferred(existing->sdf);
    }
    meshes_.erase(mesh_key);
  }
  if (input.vertex_count == 0) return false;

  const auto t0 = std::chrono::steady_clock::now();

  // Gather triangles and the local-space AABB.
  auto pos = [&](u32 v) -> Vec3 {
    const f32* p = reinterpret_cast<const f32*>(reinterpret_cast<const u8*>(input.positions) +
                                                static_cast<size_t>(v) * input.position_stride);
    return {p[0], p[1], p[2]};
  };
  base::Vector<Tri> tris;
  const u32 tri_count = input.index_count ? input.index_count / 3 : input.vertex_count / 3;
  tris.reserve(tri_count);
  Vec3 gmin{1e30f, 1e30f, 1e30f}, gmax{-1e30f, -1e30f, -1e30f};
  auto grow = [&](const Vec3& v) {
    gmin = {std::min(gmin.x, v.x), std::min(gmin.y, v.y), std::min(gmin.z, v.z)};
    gmax = {std::max(gmax.x, v.x), std::max(gmax.y, v.y), std::max(gmax.z, v.z)};
  };
  for (u32 t = 0; t < tri_count; ++t) {
    u32 i0, i1, i2;
    if (input.index_count) {
      i0 = input.indices[t * 3 + 0];
      i1 = input.indices[t * 3 + 1];
      i2 = input.indices[t * 3 + 2];
    } else {
      i0 = t * 3 + 0;
      i1 = t * 3 + 1;
      i2 = t * 3 + 2;
    }
    if (i0 >= input.vertex_count || i1 >= input.vertex_count || i2 >= input.vertex_count) continue;
    Tri tri{pos(i0), pos(i1), pos(i2)};
    grow(tri.a);
    grow(tri.b);
    grow(tri.c);
    tris.push_back(tri);
  }
  if (tris.empty()) return false;

  const Vec3 ext = gmax - gmin;
  const f32 maxext = std::max({ext.x, ext.y, ext.z, 1e-4f});
  const Vec3 center = (gmin + gmax) * 0.5f;
  constexpr int kPad = 2;  // voxels of padding beyond the geometry AABB
  f32 voxel = maxext / 36.0f;
  if (voxel <= 0) voxel = 0.01f;
  u32 res[3];
  auto compute_res = [&]() {
    for (int a = 0; a < 3; ++a) {
      u32 interior = static_cast<u32>(std::ceil((&ext.x)[a] / voxel));
      res[a] = std::clamp(interior + 2u * kPad, 16u, 64u);
    }
  };
  compute_res();
  // If any axis needed clamping down to 64, grow the voxel so the volume still
  // covers the mesh (+padding), then re-derive.
  for (int a = 0; a < 3; ++a) {
    f32 span_needed = (&ext.x)[a] + 2.0f * kPad * voxel;
    if (span_needed > res[a] * voxel) voxel = span_needed / 64.0f;
  }
  compute_res();

  MeshSdf out{};
  out.voxel = voxel;
  for (int a = 0; a < 3; ++a) {
    out.res[a] = res[a];
    out.box_min[a] = (&center.x)[a] - res[a] * voxel * 0.5f;  // centred, symmetric padding
  }
  std::memcpy(out.albedo, input.albedo, sizeof(f32) * 3);
  std::memcpy(out.emissive, input.emissive, sizeof(f32) * 3);
  const Vec3 box_min{out.box_min[0], out.box_min[1], out.box_min[2]};
  const Vec3 box_ext{res[0] * voxel, res[1] * voxel, res[2] * voxel};

  // Uniform triangle grid, ~3 voxels per cell (a handful of tris per cell).
  Grid grid;
  BuildGrid(grid, tris, box_min, box_ext, voxel * 3.0f);

  const size_t voxel_count = static_cast<size_t>(res[0]) * res[1] * res[2];
  base::Vector<f32> field;
  field.resize(voxel_count, 0.0f);
  const f32 far_dist = maxext;  // clamp distance to the mesh extent

  // Per-voxel: exact unsigned distance via grid ring search + 3-axis ray-parity
  // sign. Parallelised over z-slices (disjoint output ranges, no contention).
  auto solve_slice = [&](u32 z0, u32 z1) {
    const int cells_diag = grid.n[0] + grid.n[1] + grid.n[2];
    for (u32 z = z0; z < z1; ++z) {
      for (u32 y = 0; y < res[1]; ++y) {
        for (u32 x = 0; x < res[0]; ++x) {
          Vec3 p{box_min.x + (x + 0.5f) * voxel, box_min.y + (y + 0.5f) * voxel,
                 box_min.z + (z + 0.5f) * voxel};
          int cx = grid.ClampCell(p.x, 0), cy = grid.ClampCell(p.y, 1), cz = grid.ClampCell(p.z, 2);
          // Ring-expanding closest-triangle search.
          f32 best_sq = far_dist * far_dist;
          for (int r = 0; r <= cells_diag; ++r) {
            const f32 ring_min = (r - 1) * grid.cell;  // nearest a ring-r cell can be
            if (r > 0 && ring_min * ring_min > best_sq) break;
            for (int dz = -r; dz <= r; ++dz) {
              int z2 = cz + dz;
              if (z2 < 0 || z2 >= grid.n[2]) continue;
              for (int dy = -r; dy <= r; ++dy) {
                int y2 = cy + dy;
                if (y2 < 0 || y2 >= grid.n[1]) continue;
                for (int dx = -r; dx <= r; ++dx) {
                  // Only the shell of the r-cube (interior handled at smaller r).
                  if (r > 0 && std::abs(dx) != r && std::abs(dy) != r && std::abs(dz) != r) continue;
                  int x2 = cx + dx;
                  if (x2 < 0 || x2 >= grid.n[0]) continue;
                  int ci = grid.Index(x2, y2, z2);
                  for (u32 e = grid.start[ci]; e < grid.start[ci + 1]; ++e) {
                    f32 d = PointTriDistanceSq(p, tris[grid.tri_ids[e]]);
                    if (d < best_sq) best_sq = d;
                  }
                }
              }
            }
          }
          f32 dist = std::sqrt(best_sq);

          // Sign: 3-axis ray parity. An axis-aligned ray stays in one cell row,
          // so we only test that row; a hit counts once (cell-ownership dedup).
          int inside_votes = 0;
          for (u32 axis = 0; axis < 3; ++axis) {
            int crossings = 0;
            int base_cell[3] = {cx, cy, cz};
            for (int step = base_cell[axis]; step < grid.n[axis]; ++step) {
              int cell3[3] = {cx, cy, cz};
              cell3[axis] = step;
              int ci = grid.Index(cell3[0], cell3[1], cell3[2]);
              for (u32 e = grid.start[ci]; e < grid.start[ci + 1]; ++e) {
                f32 s;
                const Tri& tri = tris[grid.tri_ids[e]];
                if (!AxisRayCross(p, axis, tri, s)) continue;
                // Dedup: only count the crossing if its hit point falls in the
                // current marched cell along the ray axis.
                f32 hit_axis = (&p.x)[axis] + s;
                int hit_cell = grid.ClampCell(hit_axis, axis);
                if (hit_cell == step) crossings++;
              }
            }
            if (crossings & 1) inside_votes++;
          }
          if (inside_votes >= 2) dist = -dist;
          field[(static_cast<size_t>(z) * res[1] + y) * res[0] + x] = dist;
        }
      }
    }
  };

  // Chunk the z range across the job system; small meshes run inline.
  const u32 workers = std::max(1u, jobs_.thread_count());
  if (res[2] <= 2 || workers == 1) {
    solve_slice(0, res[2]);
  } else {
    std::atomic<u32> done{0};
    const u32 chunks = std::min(workers * 2, res[2]);
    const u32 per = (res[2] + chunks - 1) / chunks;
    u32 submitted = 0;
    for (u32 c = 0; c < chunks; ++c) {
      u32 z0 = c * per, z1 = std::min(z0 + per, res[2]);
      if (z0 >= z1) break;
      ++submitted;
      jobs_.Submit([&solve_slice, z0, z1, &done]() {
        solve_slice(z0, z1);
        done.fetch_add(1, std::memory_order_release);
      });
    }
    jobs_.WaitIdle();
    (void)submitted;
  }

  out.sdf = device_.CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8*>(field.data()), field.size() * sizeof(f32)),
      kBufferUsageStorage);
  if (!out.sdf) {
    RX_WARN("sdf: mesh buffer creation failed (key {:#x})", mesh_key);
    return false;
  }

  const auto t1 = std::chrono::steady_clock::now();
  out.gen_ms = std::chrono::duration<f32, std::milli>(t1 - t0).count();
  last_gen_ms_ = out.gen_ms;
  total_gen_ms_ += out.gen_ms;
  total_bytes_ += out.sdf.size;
  meshes_.insert(mesh_key, out);
  RX_INFO("sdf mesh {:#x}: {}x{}x{} ({} tris) in {:.1f} ms; total {} meshes {:.1f} ms {} KB",
          mesh_key, res[0], res[1], res[2], tris.size(), out.gen_ms, meshes_.size(), total_gen_ms_,
          total_bytes_ / 1024);
  return true;
}

}  // namespace rx::render
