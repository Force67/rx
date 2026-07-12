#include "asset/simplify.h"

#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace rx::asset {
namespace {

// Symmetric 4x4 quadric, 10 unique coefficients.
struct Quadric {
  f64 a2 = 0, ab = 0, ac = 0, ad = 0;
  f64 b2 = 0, bc = 0, bd = 0;
  f64 c2 = 0, cd = 0;
  f64 d2 = 0;

  void AddPlane(f64 a, f64 b, f64 c, f64 d) {
    a2 += a * a; ab += a * b; ac += a * c; ad += a * d;
    b2 += b * b; bc += b * c; bd += b * d;
    c2 += c * c; cd += c * d;
    d2 += d * d;
  }
  void Add(const Quadric& q) {
    a2 += q.a2; ab += q.ab; ac += q.ac; ad += q.ad;
    b2 += q.b2; bc += q.bc; bd += q.bd;
    c2 += q.c2; cd += q.cd;
    d2 += q.d2;
  }
  f64 Evaluate(const Vec3& p) const {
    f64 x = p.x, y = p.y, z = p.z;
    return a2 * x * x + 2 * ab * x * y + 2 * ac * x * z + 2 * ad * x +
           b2 * y * y + 2 * bc * y * z + 2 * bd * y +
           c2 * z * z + 2 * cd * z + d2;
  }
};

struct Candidate {
  f64 cost;
  u32 from, to;
  u32 stamp;  // from-vertex version when queued; stale entries are skipped
  bool operator<(const Candidate& other) const { return cost > other.cost; }  // min-heap
};

u64 EdgeKey(u32 a, u32 b) {
  return (static_cast<u64>(std::min(a, b)) << 32) | std::max(a, b);
}

// The actual collapse loop, over a DENSE vertex range: every scratch array is
// sized by vertex_count, so callers must pass a compacted submesh (the public
// wrapper below does). Called once per cluster group during the DAG build -
// sizing scratch by the whole source mesh instead made thousands of group
// calls over a multi-million-vertex mesh spend all their time in allocation.
std::vector<u32> SimplifyDense(const Vec3* positions, u32 vertex_count, const u32* indices,
                               u32 index_count, u32 target_index_count, const u8* locked,
                               f32* out_error) {
  std::vector<u32> result(indices, indices + index_count);

  // Union-find style remap: collapsed vertices forward to their target.
  std::vector<u32> remap(vertex_count);
  for (u32 i = 0; i < vertex_count; ++i) remap[i] = i;
  auto resolve = [&](u32 v) {
    while (remap[v] != v) v = remap[v];
    return v;
  };

  std::vector<Quadric> quadrics(vertex_count);
  std::vector<u32> version(vertex_count, 0);
  // Adjacency: triangles per vertex (triangle ids into result/3).
  std::vector<std::vector<u32>> vertex_tris(vertex_count);
  u32 live_triangles = index_count / 3;
  std::vector<bool> tri_dead(live_triangles, false);

  for (u32 t = 0; t < index_count / 3; ++t) {
    u32 i0 = result[t * 3], i1 = result[t * 3 + 1], i2 = result[t * 3 + 2];
    Vec3 p0 = positions[i0], p1 = positions[i1], p2 = positions[i2];
    Vec3 n = Cross(p1 - p0, p2 - p0);
    f64 len = std::sqrt(static_cast<f64>(n.x) * n.x + static_cast<f64>(n.y) * n.y +
                        static_cast<f64>(n.z) * n.z);
    if (len < 1e-12) {
      tri_dead[t] = true;
      --live_triangles;
      continue;
    }
    f64 a = n.x / len, b = n.y / len, c = n.z / len;
    f64 d = -(a * p0.x + b * p0.y + c * p0.z);
    // Area-weighted plane quadric on all three corners.
    Quadric q;
    q.AddPlane(a, b, c, d);
    quadrics[i0].Add(q);
    quadrics[i1].Add(q);
    quadrics[i2].Add(q);
    vertex_tris[i0].push_back(t);
    vertex_tris[i1].push_back(t);
    vertex_tris[i2].push_back(t);
  }

  // Border edges of the SUBMESH itself (used by one triangle only) also lock:
  // collapsing them would erode the patch outline even without a lock array.
  std::unordered_map<u64, u32> edge_use;
  for (u32 t = 0; t < index_count / 3; ++t) {
    if (tri_dead[t]) continue;
    for (u32 e = 0; e < 3; ++e) {
      ++edge_use[EdgeKey(result[t * 3 + e], result[t * 3 + (e + 1) % 3])];
    }
  }
  std::vector<u8> border(vertex_count, 0);
  for (const auto& entry : edge_use) {
    if (entry.second == 1) {
      border[static_cast<u32>(entry.first >> 32)] = 1;
      border[static_cast<u32>(entry.first & 0xffffffffu)] = 1;
    }
  }
  auto is_locked = [&](u32 v) { return (locked && locked[v]) || border[v]; };

  std::priority_queue<Candidate> heap;
  auto push_edge = [&](u32 a, u32 b) {
    if (is_locked(a)) return;  // half-edge collapse a -> b
    Quadric q = quadrics[a];
    q.Add(quadrics[b]);
    f64 cost = std::max(q.Evaluate(positions[b]), 0.0);
    heap.push({cost, a, b, version[a]});
  };
  {
    std::unordered_set<u64> seen;
    for (u32 t = 0; t < index_count / 3; ++t) {
      if (tri_dead[t]) continue;
      for (u32 e = 0; e < 3; ++e) {
        u32 a = result[t * 3 + e], b = result[t * 3 + (e + 1) % 3];
        if (seen.insert(EdgeKey(a, b)).second) {
          push_edge(a, b);
          push_edge(b, a);
        }
      }
    }
  }

  f64 max_cost = 0.0;
  while (live_triangles * 3 > target_index_count && !heap.empty()) {
    Candidate top = heap.top();
    heap.pop();
    u32 a = top.from, b = resolve(top.to);
    if (remap[a] != a || top.stamp != version[a] || a == b) continue;  // stale
    if (is_locked(a)) continue;

    // Triangle-flip guard: collapsing a onto b must not invert any surviving
    // triangle around a.
    bool flips = false;
    for (u32 t : vertex_tris[a]) {
      if (tri_dead[t]) continue;
      u32 v[3] = {resolve(result[t * 3]), resolve(result[t * 3 + 1]), resolve(result[t * 3 + 2])};
      // Skip triangles that die with the collapse (contain both a and b).
      if ((v[0] == b || v[1] == b || v[2] == b)) continue;
      Vec3 p[3], q[3];
      for (u32 k = 0; k < 3; ++k) {
        p[k] = positions[v[k]];
        q[k] = positions[v[k] == a ? b : v[k]];
      }
      Vec3 n0 = Cross(p[1] - p[0], p[2] - p[0]);
      Vec3 n1 = Cross(q[1] - q[0], q[2] - q[0]);
      if (Dot(n0, n1) <= 0.0f) {
        flips = true;
        break;
      }
    }
    if (flips) continue;

    // Commit: a forwards to b; b absorbs a's quadric and adjacency.
    remap[a] = b;
    ++version[a];
    quadrics[b].Add(quadrics[a]);
    max_cost = std::max(max_cost, top.cost);
    for (u32 t : vertex_tris[a]) {
      if (tri_dead[t]) continue;
      u32 v[3] = {resolve(result[t * 3]), resolve(result[t * 3 + 1]), resolve(result[t * 3 + 2])};
      if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) {
        tri_dead[t] = true;
        --live_triangles;
        continue;
      }
      vertex_tris[b].push_back(t);
    }
    // Refresh collapse candidates around b.
    std::unordered_set<u32> neighbors;
    for (u32 t : vertex_tris[b]) {
      if (tri_dead[t]) continue;
      for (u32 k = 0; k < 3; ++k) {
        u32 v = resolve(result[t * 3 + k]);
        if (v != b) neighbors.insert(v);
      }
    }
    for (u32 n : neighbors) {
      push_edge(n, b);
      push_edge(b, n);
    }
  }

  std::vector<u32> out;
  out.reserve(live_triangles * 3);
  for (u32 t = 0; t < index_count / 3; ++t) {
    if (tri_dead[t]) continue;
    u32 v0 = resolve(result[t * 3]), v1 = resolve(result[t * 3 + 1]),
        v2 = resolve(result[t * 3 + 2]);
    if (v0 == v1 || v1 == v2 || v0 == v2) continue;
    out.push_back(v0);
    out.push_back(v1);
    out.push_back(v2);
  }
  if (out_error) *out_error = static_cast<f32>(std::sqrt(std::max(max_cost, 0.0)));
  return out;
}

}  // namespace

std::vector<u32> SimplifyIndices(const Vec3* positions, u32 vertex_count, const u32* indices,
                                 u32 index_count, u32 target_index_count, const u8* locked,
                                 f32* out_error) {
  if (out_error) *out_error = 0.0f;
  if (index_count <= target_index_count || index_count < 12) {
    return std::vector<u32>(indices, indices + index_count);
  }
  (void)vertex_count;

  // Compact to the vertices this submesh actually references, simplify in
  // that dense space, then map the survivors back to source indices.
  std::unordered_map<u32, u32> to_local;
  to_local.reserve(index_count);
  std::vector<u32> to_global;
  std::vector<Vec3> local_positions;
  std::vector<u8> local_locked;
  std::vector<u32> local_indices(index_count);
  for (u32 i = 0; i < index_count; ++i) {
    u32 g = indices[i];
    auto [it, inserted] = to_local.emplace(g, static_cast<u32>(to_global.size()));
    if (inserted) {
      to_global.push_back(g);
      local_positions.push_back(positions[g]);
      local_locked.push_back(locked ? locked[g] : 0);
    }
    local_indices[i] = it->second;
  }

  std::vector<u32> out = SimplifyDense(local_positions.data(),
                                       static_cast<u32>(to_global.size()),
                                       local_indices.data(), index_count,
                                       target_index_count, local_locked.data(), out_error);
  for (u32& v : out) v = to_global[v];
  return out;
}

}  // namespace rx::asset
