#include "physics/cloth_collision.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <numeric>

namespace rx::physics::detail {
namespace {

constexpr f32 kMinAreaTwice = 1.0e-8f;

struct Edge {
  u32 a;
  u32 b;
  bool forward;
};

struct FaceKey {
  u32 a;
  u32 b;
  u32 c;
};

struct Neighbor {
  u32 vertex;
  u32 neighbor;
};

struct VertexLink {
  u32 vertex;
  u32 a;
  u32 b;
};

bool SameEdge(const Edge& a, const Edge& b) { return a.a == b.a && a.b == b.b; }

bool SameFace(const FaceKey& a, const FaceKey& b) {
  return a.a == b.a && a.b == b.b && a.c == b.c;
}

bool IsFinite(const Vec3& p) {
  return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

f64 TriangleAreaTwice(const Vec3& a, const Vec3& b, const Vec3& c) {
  const f64 abx = static_cast<f64>(b.x) - a.x;
  const f64 aby = static_cast<f64>(b.y) - a.y;
  const f64 abz = static_cast<f64>(b.z) - a.z;
  const f64 acx = static_cast<f64>(c.x) - a.x;
  const f64 acy = static_cast<f64>(c.y) - a.y;
  const f64 acz = static_cast<f64>(c.z) - a.z;
  const f64 x = aby * acz - abz * acy;
  const f64 y = abz * acx - abx * acz;
  const f64 z = abx * acy - aby * acx;
  return std::sqrt(x * x + y * y + z * z);
}

f64 SignedVolumeContribution(const Vec3& a, const Vec3& b, const Vec3& c) {
  const f64 cross_x = static_cast<f64>(b.y) * c.z - static_cast<f64>(b.z) * c.y;
  const f64 cross_y = static_cast<f64>(b.z) * c.x - static_cast<f64>(b.x) * c.z;
  const f64 cross_z = static_cast<f64>(b.x) * c.y - static_cast<f64>(b.y) * c.x;
  return (static_cast<f64>(a.x) * cross_x + static_cast<f64>(a.y) * cross_y +
          static_cast<f64>(a.z) * cross_z) /
         6.0;
}

Edge MakeEdge(u32 a, u32 b) {
  return a < b ? Edge{a, b, true} : Edge{b, a, false};
}

FaceKey MakeFaceKey(u32 a, u32 b, u32 c) {
  if (a > b) std::swap(a, b);
  if (b > c) std::swap(b, c);
  if (a > b) std::swap(a, b);
  return {a, b, c};
}

bool IsTopologyNeighbor(const ClothTopology& topology, u32 vertex, u32 other) {
  const u32 begin = topology.neighbor_offsets[vertex];
  const u32 end = topology.neighbor_offsets[vertex + 1];
  return std::binary_search(topology.neighbors.begin() + begin,
                            topology.neighbors.begin() + end, other);
}

Vec3 ClosestPointOnSegment(const Vec3& p, const Vec3& a, const Vec3& b,
                           f32* t) {
  const Vec3 ab = b - a;
  const f32 denominator = Dot(ab, ab);
  *t = denominator > 1.0e-12f
           ? std::clamp(Dot(p - a, ab) / denominator, 0.0f, 1.0f)
           : 0;
  return a + ab * *t;
}

// Closest point and barycentrics from Real-Time Collision Detection, section
// 5.1.5, with a segment fallback for triangles that collapse at runtime.
Vec3 ClosestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b,
                            const Vec3& c, f32* wa, f32* wb, f32* wc) {
  const Vec3 ab = b - a;
  const Vec3 ac = c - a;
  if (Dot(Cross(ab, ac), Cross(ab, ac)) < 1.0e-14f) {
    f32 tab = 0, tbc = 0, tca = 0;
    const Vec3 qab = ClosestPointOnSegment(p, a, b, &tab);
    const Vec3 qbc = ClosestPointOnSegment(p, b, c, &tbc);
    const Vec3 qca = ClosestPointOnSegment(p, c, a, &tca);
    const f32 dab = Dot(p - qab, p - qab);
    const f32 dbc = Dot(p - qbc, p - qbc);
    const f32 dca = Dot(p - qca, p - qca);
    if (dab <= dbc && dab <= dca) {
      *wa = 1 - tab;
      *wb = tab;
      *wc = 0;
      return qab;
    }
    if (dbc <= dca) {
      *wa = 0;
      *wb = 1 - tbc;
      *wc = tbc;
      return qbc;
    }
    *wa = tca;
    *wb = 0;
    *wc = 1 - tca;
    return qca;
  }
  const Vec3 ap = p - a;
  const f32 d1 = Dot(ab, ap);
  const f32 d2 = Dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) {
    *wa = 1;
    *wb = *wc = 0;
    return a;
  }

  const Vec3 bp = p - b;
  const f32 d3 = Dot(ab, bp);
  const f32 d4 = Dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) {
    *wb = 1;
    *wa = *wc = 0;
    return b;
  }

  const f32 vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    const f32 v = d1 / (d1 - d3);
    *wa = 1 - v;
    *wb = v;
    *wc = 0;
    return a + ab * v;
  }

  const Vec3 cp = p - c;
  const f32 d5 = Dot(ab, cp);
  const f32 d6 = Dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) {
    *wc = 1;
    *wa = *wb = 0;
    return c;
  }

  const f32 vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    const f32 w = d2 / (d2 - d6);
    *wa = 1 - w;
    *wb = 0;
    *wc = w;
    return a + ac * w;
  }

  const f32 va = d3 * d6 - d5 * d4;
  if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
    const f32 w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    *wa = 0;
    *wb = 1 - w;
    *wc = w;
    return b + (c - b) * w;
  }

  const f32 inv = 1.0f / (va + vb + vc);
  *wb = vb * inv;
  *wc = vc * inv;
  *wa = 1 - *wb - *wc;
  return a + ab * *wb + ac * *wc;
}

struct VertexFaceContact {
  Vec3 normal;
  f32 a = 0;
  f32 b = 0;
  f32 c = 0;
};

f64 Dot64(const Vec3& a, const Vec3& b) {
  return static_cast<f64>(a.x) * b.x + static_cast<f64>(a.y) * b.y +
         static_cast<f64>(a.z) * b.z;
}

u32 FindCubicRoots(f64 c0, f64 c1, f64 c2, f64 c3, f32 roots[4]) {
  const f64 scale =
      std::abs(c0) + std::abs(c1) + std::abs(c2) + std::abs(c3) + 1.0;
  const f64 epsilon = scale * 1.0e-10;
  f64 points[4] = {0, 1, 0, 0};
  u32 point_count = 2;
  const f64 da = 3 * c3;
  const f64 db = 2 * c2;
  const f64 dc = c1;
  if (std::abs(da) > epsilon) {
    const f64 discriminant = db * db - 4 * da * dc;
    if (discriminant >= 0) {
      const f64 root = std::sqrt(discriminant);
      const f64 t0 = (-db - root) / (2 * da);
      const f64 t1 = (-db + root) / (2 * da);
      if (t0 > 0 && t0 < 1 && point_count < 4) points[point_count++] = t0;
      if (t1 > 0 && t1 < 1 && point_count < 4) points[point_count++] = t1;
    }
  } else if (std::abs(db) > epsilon) {
    const f64 t = -dc / db;
    if (t > 0 && t < 1 && point_count < 4) points[point_count++] = t;
  }
  for (u32 i = 1; i < point_count; ++i) {
    for (u32 j = i; j > 0 && points[j] < points[j - 1]; --j)
      std::swap(points[j], points[j - 1]);
  }
  auto evaluate = [&](f64 t) { return ((c3 * t + c2) * t + c1) * t + c0; };
  u32 root_count = 0;
  auto add_root = [&](f64 root) {
    if (root < -epsilon || root > 1 + epsilon) return;
    const f32 value = static_cast<f32>(std::clamp(root, 0.0, 1.0));
    if (root_count < 4 && (root_count == 0 ||
                           std::abs(value - roots[root_count - 1]) > 1.0e-5f)) {
      roots[root_count++] = value;
    }
  };
  for (u32 i = 0; i < point_count; ++i) {
    if (std::abs(evaluate(points[i])) <= epsilon) add_root(points[i]);
    if (i + 1 == point_count) continue;
    f64 low = points[i], high = points[i + 1];
    f64 low_value = evaluate(low);
    const f64 high_value = evaluate(high);
    if ((low_value < 0) == (high_value < 0)) continue;
    for (u32 iteration = 0; iteration < 48; ++iteration) {
      const f64 middle = 0.5 * (low + high);
      const f64 middle_value = evaluate(middle);
      if ((low_value < 0) == (middle_value < 0)) {
        low = middle;
        low_value = middle_value;
      } else {
        high = middle;
      }
    }
    add_root(0.5 * (low + high));
  }
  std::sort(roots, roots + root_count);
  return root_count;
}

u32 FindCoplanarityRoots(const Vec3& p0, const Vec3& p1, const Vec3& a0,
                         const Vec3& a1, const Vec3& b0, const Vec3& b1,
                         const Vec3& c0, const Vec3& c1, f32 roots[4]) {
  const Vec3 r0 = p0 - a0;
  const Vec3 r1 = (p1 - p0) - (a1 - a0);
  const Vec3 u0 = b0 - a0;
  const Vec3 u1 = (b1 - b0) - (a1 - a0);
  const Vec3 v0 = c0 - a0;
  const Vec3 v1 = (c1 - c0) - (a1 - a0);
  const Vec3 cross0 = Cross(u0, v0);
  const Vec3 cross1 = Cross(u1, v0) + Cross(u0, v1);
  const Vec3 cross2 = Cross(u1, v1);
  return FindCubicRoots(
      Dot64(r0, cross0), Dot64(r1, cross0) + Dot64(r0, cross1),
      Dot64(r1, cross1) + Dot64(r0, cross2), Dot64(r1, cross2), roots);
}

bool FindVertexFaceContact(const Vec3& p0, const Vec3& p1, const Vec3& a0,
                           const Vec3& a1, const Vec3& b0, const Vec3& b1,
                           const Vec3& c0, const Vec3& c1, f32 distance,
                           VertexFaceContact* out) {
  const Vec3 vp = p1 - p0;
  const Vec3 va = a1 - a0;
  const Vec3 vb = b1 - b0;
  const Vec3 vc = c1 - c0;
  const f32 speed =
      std::max({Length(vp - va), Length(vp - vb), Length(vp - vc)});
  f32 time = 0;
  for (u32 step = 0; step < 64 && time <= 1; ++step) {
    const Vec3 p = Lerp(p0, p1, time);
    const Vec3 a = Lerp(a0, a1, time);
    const Vec3 b = Lerp(b0, b1, time);
    const Vec3 c = Lerp(c0, c1, time);
    f32 wa = 0, wb = 0, wc = 0;
    const Vec3 closest = ClosestPointOnTriangle(p, a, b, c, &wa, &wb, &wc);
    const Vec3 delta = p - closest;
    const f32 separation = Length(delta);
    if (separation <= distance) {
      Vec3 normal = separation > 1.0e-7f ? delta * (1.0f / separation)
                                         : Normalize(Cross(b - a, c - a));
      if (Length(normal) < 0.5f) return false;
      if (separation <= 1.0e-7f) {
        f32 r0a = 0, r0b = 0, r0c = 0;
        const Vec3 initial_closest =
            ClosestPointOnTriangle(p0, a0, b0, c0, &r0a, &r0b, &r0c);
        const Vec3 initial_normal = Normalize(Cross(b0 - a0, c0 - a0));
        if (Dot(p0 - initial_closest, initial_normal) < 0)
          normal = normal * -1.0f;
      }
      *out = {normal, wa, wb, wc};
      return true;
    }
    if (speed <= 1.0e-8f) return false;
    const f32 advance = (separation - distance) / speed * 0.8f;
    if (advance <= 1.0e-7f) break;
    time += advance;
  }
  f32 roots[4];
  const u32 root_count =
      FindCoplanarityRoots(p0, p1, a0, a1, b0, b1, c0, c1, roots);
  for (u32 i = 0; i < root_count; ++i) {
    const f32 root = roots[i];
    const Vec3 p = Lerp(p0, p1, root);
    const Vec3 a = Lerp(a0, a1, root);
    const Vec3 b = Lerp(b0, b1, root);
    const Vec3 c = Lerp(c0, c1, root);
    f32 wa = 0, wb = 0, wc = 0;
    const Vec3 closest = ClosestPointOnTriangle(p, a, b, c, &wa, &wb, &wc);
    if (Length(p - closest) > std::max(distance * 0.1f, 1.0e-5f)) continue;
    Vec3 normal = Normalize(Cross(b - a, c - a));
    if (Length(normal) < 0.5f) continue;
    const f32 before = std::max(root - 1.0e-4f, 0.0f);
    const Vec3 before_p = Lerp(p0, p1, before);
    const Vec3 before_a = Lerp(a0, a1, before);
    const Vec3 before_b = Lerp(b0, b1, before);
    const Vec3 before_c = Lerp(c0, c1, before);
    f32 before_wa = 0, before_wb = 0, before_wc = 0;
    const Vec3 before_closest =
        ClosestPointOnTriangle(before_p, before_a, before_b, before_c,
                               &before_wa, &before_wb, &before_wc);
    if (Dot(before_p - before_closest, normal) < 0) normal = normal * -1.0f;
    *out = {normal, wa, wb, wc};
    return true;
  }
  return false;
}

void ClosestPointsOnSegments(const Vec3& p1, const Vec3& q1, const Vec3& p2,
                             const Vec3& q2, f32* s, f32* t, Vec3* c1,
                             Vec3* c2) {
  const Vec3 d1 = q1 - p1;
  const Vec3 d2 = q2 - p2;
  const Vec3 r = p1 - p2;
  const f32 a = Dot(d1, d1);
  const f32 e = Dot(d2, d2);
  const f32 f = Dot(d2, r);
  if (a <= 1.0e-12f && e <= 1.0e-12f) {
    *s = *t = 0;
  } else if (a <= 1.0e-12f) {
    *s = 0;
    *t = std::clamp(f / e, 0.0f, 1.0f);
  } else {
    const f32 c = Dot(d1, r);
    if (e <= 1.0e-12f) {
      *t = 0;
      *s = std::clamp(-c / a, 0.0f, 1.0f);
    } else {
      const f32 b = Dot(d1, d2);
      const f32 denominator = a * e - b * b;
      *s = denominator != 0
               ? std::clamp((b * f - c * e) / denominator, 0.0f, 1.0f)
               : 0;
      *t = (b * *s + f) / e;
      if (*t < 0) {
        *t = 0;
        *s = std::clamp(-c / a, 0.0f, 1.0f);
      } else if (*t > 1) {
        *t = 1;
        *s = std::clamp((b - c) / a, 0.0f, 1.0f);
      }
    }
  }
  *c1 = p1 + d1 * *s;
  *c2 = p2 + d2 * *t;
}

struct EdgeContact {
  Vec3 normal;
  f32 a = 0;
  f32 b = 0;
};

bool FindEdgeContact(const Vec3& a0, const Vec3& a1, const Vec3& b0,
                     const Vec3& b1, const Vec3& c0, const Vec3& c1,
                     const Vec3& d0, const Vec3& d1, f32 distance,
                     EdgeContact* out) {
  const Vec3 va = a1 - a0;
  const Vec3 vb = b1 - b0;
  const Vec3 vc = c1 - c0;
  const Vec3 vd = d1 - d0;
  const f32 speed = std::max(
      {Length(va - vc), Length(va - vd), Length(vb - vc), Length(vb - vd)});
  f32 time = 0;
  for (u32 step = 0; step < 64 && time <= 1; ++step) {
    const Vec3 a = Lerp(a0, a1, time);
    const Vec3 b = Lerp(b0, b1, time);
    const Vec3 c = Lerp(c0, c1, time);
    const Vec3 d = Lerp(d0, d1, time);
    f32 edge_a = 0, edge_b = 0;
    Vec3 point_a, point_b;
    ClosestPointsOnSegments(a, b, c, d, &edge_a, &edge_b, &point_a, &point_b);
    const Vec3 delta = point_a - point_b;
    const f32 separation = Length(delta);
    if (separation <= distance) {
      Vec3 normal = separation > 1.0e-7f ? delta * (1.0f / separation)
                                         : Normalize(Cross(b - a, d - c));
      if (Length(normal) < 0.5f) return false;
      *out = {normal, edge_a, edge_b};
      return true;
    }
    if (speed <= 1.0e-8f) return false;
    const f32 advance = (separation - distance) / speed * 0.8f;
    if (advance <= 1.0e-7f) break;
    time += advance;
  }
  f32 roots[4];
  const u32 root_count =
      FindCoplanarityRoots(a0, a1, c0, c1, b0, b1, d0, d1, roots);
  for (u32 i = 0; i < root_count; ++i) {
    const f32 root = roots[i];
    const Vec3 a = Lerp(a0, a1, root);
    const Vec3 b = Lerp(b0, b1, root);
    const Vec3 c = Lerp(c0, c1, root);
    const Vec3 d = Lerp(d0, d1, root);
    f32 edge_a = 0, edge_b = 0;
    Vec3 point_a, point_b;
    ClosestPointsOnSegments(a, b, c, d, &edge_a, &edge_b, &point_a, &point_b);
    if (Length(point_a - point_b) > std::max(distance * 0.1f, 1.0e-5f))
      continue;
    Vec3 normal = Normalize(Cross(b - a, d - c));
    if (Length(normal) < 0.5f) continue;
    const f32 before = std::max(root - 1.0e-4f, 0.0f);
    f32 before_a_weight = 0, before_b_weight = 0;
    Vec3 before_a, before_b;
    ClosestPointsOnSegments(Lerp(a0, a1, before), Lerp(b0, b1, before),
                            Lerp(c0, c1, before), Lerp(d0, d1, before),
                            &before_a_weight, &before_b_weight, &before_a,
                            &before_b);
    if (Dot(before_a - before_b, normal) < 0) normal = normal * -1.0f;
    *out = {normal, edge_a, edge_b};
    return true;
  }
  return false;
}

using Bounds = ClothSelfCollisionScratch::Bounds;

Bounds EmptyBounds() {
  const f32 inf = std::numeric_limits<f32>::infinity();
  return {{inf, inf, inf}, {-inf, -inf, -inf}};
}

void Expand(Bounds* bounds, const Vec3& point) {
  bounds->low = {std::min(bounds->low.x, point.x),
                 std::min(bounds->low.y, point.y),
                 std::min(bounds->low.z, point.z)};
  bounds->high = {std::max(bounds->high.x, point.x),
                  std::max(bounds->high.y, point.y),
                  std::max(bounds->high.z, point.z)};
}

void Expand(Bounds* bounds, const Bounds& other) {
  Expand(bounds, other.low);
  Expand(bounds, other.high);
}

Bounds Expanded(Bounds bounds, f32 distance) {
  const Vec3 extent{distance, distance, distance};
  bounds.low = bounds.low - extent;
  bounds.high += extent;
  return bounds;
}

bool Overlaps(const Bounds& a, const Bounds& b) {
  return a.low.x <= b.high.x && a.high.x >= b.low.x && a.low.y <= b.high.y &&
         a.high.y >= b.low.y && a.low.z <= b.high.z && a.high.z >= b.low.z;
}

u32 BuildBvhNode(u32 begin, u32 end, ClothSelfCollisionScratch* scratch) {
  const u32 node_index = static_cast<u32>(scratch->bvh.size());
  scratch->bvh.push_back({});
  Bounds bounds = EmptyBounds();
  Bounds centroids = EmptyBounds();
  for (u32 i = begin; i < end; ++i) {
    const Bounds& primitive = scratch->primitive_bounds[scratch->primitives[i]];
    Expand(&bounds, primitive);
    Expand(&centroids, (primitive.low + primitive.high) * 0.5f);
  }
  auto& node = scratch->bvh[node_index];
  node.bounds = bounds;
  node.begin = begin;
  node.count = end - begin;
  if (node.count <= 4) return node_index;

  const Vec3 extent = centroids.high - centroids.low;
  const u32 axis = extent.y > extent.x && extent.y >= extent.z
                       ? 1
                       : (extent.z > extent.x ? 2 : 0);
  const u32 middle = (begin + end) / 2;
  auto coordinate = [&](u32 primitive) {
    const Bounds& primitive_bounds = scratch->primitive_bounds[primitive];
    const Vec3 center = (primitive_bounds.low + primitive_bounds.high) * 0.5f;
    return axis == 0 ? center.x : (axis == 1 ? center.y : center.z);
  };
  std::nth_element(scratch->primitives.begin() + begin,
                   scratch->primitives.begin() + middle,
                   scratch->primitives.begin() + end,
                   [&](u32 a, u32 b) { return coordinate(a) < coordinate(b); });
  const u32 left = BuildBvhNode(begin, middle, scratch);
  const u32 right = BuildBvhNode(middle, end, scratch);
  scratch->bvh[node_index].left = left;
  scratch->bvh[node_index].right = right;
  scratch->bvh[node_index].count = 0;
  return node_index;
}

template <typename BoundsFn>
void BuildBvh(u32 primitive_count, BoundsFn bounds_fn,
              ClothSelfCollisionScratch* scratch) {
  scratch->primitive_bounds.resize(primitive_count);
  scratch->primitives.resize(primitive_count);
  scratch->bvh.clear();
  for (u32 primitive = 0; primitive < primitive_count; ++primitive) {
    scratch->primitive_bounds[primitive] = bounds_fn(primitive);
    scratch->primitives[primitive] = primitive;
  }
  if (primitive_count > 0) {
    scratch->bvh.reserve(primitive_count * 2);
    BuildBvhNode(0, primitive_count, scratch);
  }
}

template <typename CandidateFn>
void QueryBvh(const Bounds& query, const ClothSelfCollisionScratch& scratch,
              CandidateFn candidate_fn) {
  if (scratch.bvh.empty()) return;
  // Median splits halve a u32-sized primitive range, so traversal depth stays
  // below 32; 128 leaves ample slack if leaf size changes.
  u32 stack[128];
  u32 stack_size = 1;
  stack[0] = 0;
  while (stack_size > 0) {
    const auto& node = scratch.bvh[stack[--stack_size]];
    if (!Overlaps(query, node.bounds)) continue;
    if (node.count > 0) {
      for (u32 i = node.begin; i < node.begin + node.count; ++i) {
        candidate_fn(scratch.primitives[i]);
      }
    } else {
      assert(stack_size <= 126);
      stack[stack_size++] = node.left;
      stack[stack_size++] = node.right;
    }
  }
}

}  // namespace

bool BuildClothTopology(const Vec3* positions, u32 vertex_count,
                        const u32* indices, u32 index_count,
                        ClothTopology* out) {
  if (!out || !positions || !indices || vertex_count < 3 || index_count < 3 ||
      index_count % 3 != 0) {
    return false;
  }
  for (u32 i = 0; i < vertex_count; ++i) {
    if (!IsFinite(positions[i])) return false;
  }

  base::Vector<Edge> edges;
  base::Vector<Edge> unique_edges;
  base::Vector<FaceKey> faces;
  base::Vector<Neighbor> neighbors;
  base::Vector<VertexLink> links;
  base::Vector<f32> triangle_areas;
  edges.reserve(index_count);
  faces.reserve(index_count / 3);
  neighbors.reserve(index_count * 2);
  links.reserve(index_count);
  triangle_areas.reserve(index_count / 3);
  f64 signed_volume = 0;
  for (u32 i = 0; i < index_count; i += 3) {
    const u32 a = indices[i + 0];
    const u32 b = indices[i + 1];
    const u32 c = indices[i + 2];
    if (a >= vertex_count || b >= vertex_count || c >= vertex_count || a == b ||
        b == c || c == a) {
      return false;
    }
    const f64 area_twice =
        TriangleAreaTwice(positions[a], positions[b], positions[c]);
    if (!std::isfinite(area_twice) || area_twice < kMinAreaTwice ||
        area_twice > std::numeric_limits<f32>::max()) {
      return false;
    }
    triangle_areas.push_back(static_cast<f32>(area_twice * 0.5));
    edges.push_back(MakeEdge(a, b));
    edges.push_back(MakeEdge(b, c));
    edges.push_back(MakeEdge(c, a));
    faces.push_back(MakeFaceKey(a, b, c));
    const f64 volume =
        SignedVolumeContribution(positions[a], positions[b], positions[c]);
    if (!std::isfinite(volume)) return false;
    signed_volume += volume;
    neighbors.push_back({a, b});
    neighbors.push_back({a, c});
    neighbors.push_back({b, a});
    neighbors.push_back({b, c});
    neighbors.push_back({c, a});
    neighbors.push_back({c, b});
    links.push_back({a, b, c});
    links.push_back({b, c, a});
    links.push_back({c, a, b});
  }

  std::sort(faces.begin(), faces.end(), [](const FaceKey& a, const FaceKey& b) {
    return a.a < b.a ||
           (a.a == b.a && (a.b < b.b || (a.b == b.b && a.c < b.c)));
  });
  for (size_t i = 1; i < faces.size(); ++i) {
    if (SameFace(faces[i - 1], faces[i])) return false;
  }

  std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
    return a.a < b.a || (a.a == b.a && a.b < b.b);
  });
  bool closed = true;
  f64 edge_length = 0;
  u32 edge_count = 0;
  for (size_t i = 0; i < edges.size();) {
    size_t end = i + 1;
    while (end < edges.size() && SameEdge(edges[i], edges[end])) ++end;
    if (end - i > 2) return false;
    if (end - i == 2 && edges[i].forward == edges[i + 1].forward) return false;
    closed = closed && end - i == 2;
    unique_edges.push_back(edges[i]);
    const Vec3& a = positions[edges[i].a];
    const Vec3& b = positions[edges[i].b];
    const f64 dx = static_cast<f64>(b.x) - a.x;
    const f64 dy = static_cast<f64>(b.y) - a.y;
    const f64 dz = static_cast<f64>(b.z) - a.z;
    const f64 length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (!std::isfinite(length)) return false;
    edge_length += length;
    ++edge_count;
    i = end;
  }

  std::sort(neighbors.begin(), neighbors.end(),
            [](const Neighbor& a, const Neighbor& b) {
              return a.vertex < b.vertex ||
                     (a.vertex == b.vertex && a.neighbor < b.neighbor);
            });
  neighbors.erase(std::unique(neighbors.begin(), neighbors.end(),
                              [](const Neighbor& a, const Neighbor& b) {
                                return a.vertex == b.vertex &&
                                       a.neighbor == b.neighbor;
                              }),
                  neighbors.end());

  // A manifold vertex has one connected link: a cycle in the interior or a
  // path on a boundary. This rejects bow-tie vertices where otherwise valid
  // sheets touch only at one particle.
  std::sort(links.begin(), links.end(),
            [](const VertexLink& a, const VertexLink& b) {
              return a.vertex < b.vertex;
            });
  size_t link_at = 0;
  for (u32 vertex = 0; vertex < vertex_count; ++vertex) {
    const size_t begin = link_at;
    while (link_at < links.size() && links[link_at].vertex == vertex) ++link_at;
    if (begin == link_at) return false;
    base::Vector<u32> local_vertices;
    local_vertices.reserve((link_at - begin) * 2);
    for (size_t i = begin; i < link_at; ++i) {
      local_vertices.push_back(links[i].a);
      local_vertices.push_back(links[i].b);
    }
    std::sort(local_vertices.begin(), local_vertices.end());
    local_vertices.erase(
        std::unique(local_vertices.begin(), local_vertices.end()),
        local_vertices.end());
    base::Vector<u32> parent;
    base::Vector<u32> degree;
    parent.resize(local_vertices.size());
    degree.resize(local_vertices.size());
    std::iota(parent.begin(), parent.end(), 0u);
    std::fill(degree.begin(), degree.end(), 0);
    auto root = [&](u32 node) {
      while (parent[node] != node) {
        parent[node] = parent[parent[node]];
        node = parent[node];
      }
      return node;
    };
    for (size_t i = begin; i < link_at; ++i) {
      const u32 a =
          static_cast<u32>(std::lower_bound(local_vertices.begin(),
                                            local_vertices.end(), links[i].a) -
                           local_vertices.begin());
      const u32 b =
          static_cast<u32>(std::lower_bound(local_vertices.begin(),
                                            local_vertices.end(), links[i].b) -
                           local_vertices.begin());
      if (++degree[a] > 2 || ++degree[b] > 2) return false;
      const u32 ra = root(a), rb = root(b);
      if (ra != rb) parent[rb] = ra;
    }
    u32 roots = 0;
    u32 ends = 0;
    for (u32 i = 0; i < local_vertices.size(); ++i) {
      if (root(i) == i) ++roots;
      if (degree[i] == 1) ++ends;
      if (degree[i] == 0) return false;
    }
    if (roots != 1 || (ends != 0 && ends != 2)) return false;
  }

  base::Vector<u32> component_parent;
  component_parent.resize(vertex_count);
  std::iota(component_parent.begin(), component_parent.end(), 0u);
  auto component_root = [&](u32 node) {
    while (component_parent[node] != node) {
      component_parent[node] = component_parent[component_parent[node]];
      node = component_parent[node];
    }
    return node;
  };
  for (const Edge& edge : unique_edges) {
    const u32 a = component_root(edge.a), b = component_root(edge.b);
    if (a != b) component_parent[b] = a;
  }
  u32 component_count = 0;
  for (u32 i = 0; i < vertex_count; ++i) {
    if (component_root(i) == i) ++component_count;
  }

  ClothTopology result;
  result.indices.assign(indices, indices + index_count);
  result.triangle_areas = std::move(triangle_areas);
  result.edges.reserve(unique_edges.size() * 2);
  for (const Edge& edge : unique_edges) {
    result.edges.push_back(edge.a);
    result.edges.push_back(edge.b);
  }
  result.neighbor_offsets.resize(static_cast<size_t>(vertex_count) + 1);
  size_t at = 0;
  for (u32 vertex = 0; vertex < vertex_count; ++vertex) {
    result.neighbor_offsets[vertex] = static_cast<u32>(result.neighbors.size());
    while (at < neighbors.size() && neighbors[at].vertex == vertex) {
      result.neighbors.push_back(neighbors[at].neighbor);
      ++at;
    }
  }
  result.neighbor_offsets[vertex_count] =
      static_cast<u32>(result.neighbors.size());
  const f64 average_edge_length = edge_count > 0 ? edge_length / edge_count : 0;
  if (!std::isfinite(average_edge_length) ||
      average_edge_length > std::numeric_limits<f32>::max() ||
      !std::isfinite(signed_volume) ||
      std::abs(signed_volume) > std::numeric_limits<f32>::max()) {
    return false;
  }
  result.average_edge_length = static_cast<f32>(average_edge_length);
  result.signed_volume = static_cast<f32>(signed_volume);
  result.component_count = component_count;
  result.closed = closed;
  *out = std::move(result);
  return true;
}

u32 SolveClothSelfCollision(const ClothTopology& topology,
                            const ClothSelfCollisionConfig& config,
                            const base::Vector<Vec3>& positions,
                            base::Vector<Vec3>* velocities,
                            const base::Vector<f32>& inverse_masses, f32 dt,
                            ClothSelfCollisionScratch* scratch) {
  if (!velocities || !scratch || dt <= 0 || config.distance <= 0 ||
      positions.size() != velocities->size() ||
      positions.size() != inverse_masses.size() ||
      topology.neighbor_offsets.size() != positions.size() + 1) {
    return 0;
  }

  const f32 relaxation = std::max(0.0f, std::min(config.relaxation, 1.0f));
  const f32 max_correction = std::max(config.max_velocity, 0.01f) * dt;
  const u32 triangle_count = static_cast<u32>(topology.indices.size() / 3);
  scratch->predicted.resize(positions.size());
  for (size_t i = 0; i < positions.size(); ++i) {
    if (!IsFinite(positions[i]) || !IsFinite((*velocities)[i]) ||
        !std::isfinite(inverse_masses[i]) || inverse_masses[i] < 0) {
      return 0;
    }
    scratch->predicted[i] = positions[i] + (*velocities)[i] * dt;
    if (!IsFinite(scratch->predicted[i])) return 0;
  }

  u32 total_contacts = 0;
  for (u32 iteration = 0; iteration < std::max(config.iterations, 1u);
       ++iteration) {
    BuildBvh(
        triangle_count,
        [&](u32 triangle) {
          Bounds bounds = EmptyBounds();
          for (u32 corner = 0; corner < 3; ++corner) {
            const u32 vertex = topology.indices[triangle * 3 + corner];
            Expand(&bounds, positions[vertex]);
            Expand(&bounds, scratch->predicted[vertex]);
          }
          return bounds;
        },
        scratch);
    u32 iteration_contacts = 0;
    for (u32 vertex = 0; vertex < positions.size(); ++vertex) {
      auto solve_candidate = [&](u32 triangle) {
        const u32 ia = topology.indices[triangle * 3 + 0];
        const u32 ib = topology.indices[triangle * 3 + 1];
        const u32 ic = topology.indices[triangle * 3 + 2];
        if (vertex == ia || vertex == ib || vertex == ic ||
            IsTopologyNeighbor(topology, vertex, ia) ||
            IsTopologyNeighbor(topology, vertex, ib) ||
            IsTopologyNeighbor(topology, vertex, ic)) {
          return;
        }

        VertexFaceContact contact;
        if (!FindVertexFaceContact(
                positions[vertex], scratch->predicted[vertex], positions[ia],
                scratch->predicted[ia], positions[ib], scratch->predicted[ib],
                positions[ic], scratch->predicted[ic], config.distance,
                &contact)) {
          return;
        }
        const Vec3 triangle_point = scratch->predicted[ia] * contact.a +
                                    scratch->predicted[ib] * contact.b +
                                    scratch->predicted[ic] * contact.c;
        const f32 separation =
            Dot(scratch->predicted[vertex] - triangle_point, contact.normal);
        if (separation >= config.distance) return;

        const f32 wv = inverse_masses[vertex];
        const f32 wa = inverse_masses[ia];
        const f32 wb = inverse_masses[ib];
        const f32 wc = inverse_masses[ic];
        const f32 denominator = wv + wa * contact.a * contact.a +
                                wb * contact.b * contact.b +
                                wc * contact.c * contact.c;
        if (denominator <= 1.0e-8f) return;
        Vec3 correction = contact.normal * ((config.distance - separation) *
                                            relaxation / denominator);
        const f32 correction_length = Length(correction);
        if (!IsFinite(correction) || !std::isfinite(correction_length)) return;
        if (correction_length > max_correction) {
          correction = correction * (max_correction / correction_length);
        }
        scratch->predicted[vertex] += correction * wv;
        scratch->predicted[ia] += correction * (-wa * contact.a);
        scratch->predicted[ib] += correction * (-wb * contact.b);
        scratch->predicted[ic] += correction * (-wc * contact.c);
        ++iteration_contacts;
      };
      Bounds query = EmptyBounds();
      Expand(&query, positions[vertex]);
      Expand(&query, scratch->predicted[vertex]);
      QueryBvh(Expanded(query, config.distance), *scratch, solve_candidate);
    }

    const u32 edge_count = static_cast<u32>(topology.edges.size() / 2);
    BuildBvh(
        edge_count,
        [&](u32 edge) {
          Bounds bounds = EmptyBounds();
          for (u32 endpoint = 0; endpoint < 2; ++endpoint) {
            const u32 vertex = topology.edges[edge * 2 + endpoint];
            Expand(&bounds, positions[vertex]);
            Expand(&bounds, scratch->predicted[vertex]);
          }
          return bounds;
        },
        scratch);
    for (u32 edge = 0; edge < edge_count; ++edge) {
      const u32 ia = topology.edges[edge * 2 + 0];
      const u32 ib = topology.edges[edge * 2 + 1];
      auto solve_edge = [&](u32 other) {
        if (other <= edge) return;
        const u32 ic = topology.edges[other * 2 + 0];
        const u32 id = topology.edges[other * 2 + 1];
        if (ia == ic || ia == id || ib == ic || ib == id ||
            IsTopologyNeighbor(topology, ia, ic) ||
            IsTopologyNeighbor(topology, ia, id) ||
            IsTopologyNeighbor(topology, ib, ic) ||
            IsTopologyNeighbor(topology, ib, id)) {
          return;
        }
        EdgeContact contact;
        if (!FindEdgeContact(positions[ia], scratch->predicted[ia],
                             positions[ib], scratch->predicted[ib],
                             positions[ic], scratch->predicted[ic],
                             positions[id], scratch->predicted[id],
                             config.distance, &contact)) {
          return;
        }
        const f32 ea = 1 - contact.a;
        const f32 eb = contact.a;
        const f32 ec = 1 - contact.b;
        const f32 ed = contact.b;
        const Vec3 point_a =
            scratch->predicted[ia] * ea + scratch->predicted[ib] * eb;
        const Vec3 point_b =
            scratch->predicted[ic] * ec + scratch->predicted[id] * ed;
        const f32 separation = Dot(point_a - point_b, contact.normal);
        if (separation >= config.distance) return;
        const f32 wa = inverse_masses[ia];
        const f32 wb = inverse_masses[ib];
        const f32 wc = inverse_masses[ic];
        const f32 wd = inverse_masses[id];
        const f32 denominator =
            wa * ea * ea + wb * eb * eb + wc * ec * ec + wd * ed * ed;
        if (denominator <= 1.0e-8f) return;
        Vec3 correction = contact.normal * ((config.distance - separation) *
                                            relaxation / denominator);
        const f32 correction_length = Length(correction);
        if (!IsFinite(correction) || !std::isfinite(correction_length)) return;
        if (correction_length > max_correction) {
          correction = correction * (max_correction / correction_length);
        }
        scratch->predicted[ia] += correction * (wa * ea);
        scratch->predicted[ib] += correction * (wb * eb);
        scratch->predicted[ic] += correction * (-wc * ec);
        scratch->predicted[id] += correction * (-wd * ed);
        ++iteration_contacts;
      };

      Bounds query = EmptyBounds();
      Expand(&query, positions[ia]);
      Expand(&query, scratch->predicted[ia]);
      Expand(&query, positions[ib]);
      Expand(&query, scratch->predicted[ib]);
      QueryBvh(Expanded(query, config.distance), *scratch, solve_edge);
    }
    total_contacts += iteration_contacts;
    if (iteration_contacts == 0) break;
  }

  const f32 inv_dt = 1.0f / dt;
  for (size_t i = 0; i < positions.size(); ++i) {
    const Vec3 unprojected = positions[i] + (*velocities)[i] * dt;
    Vec3 velocity =
        (*velocities)[i] + (scratch->predicted[i] - unprojected) * inv_dt;
    if (!IsFinite(velocity)) continue;
    const f32 speed = Length(velocity);
    if (speed > config.max_velocity)
      velocity = velocity * (config.max_velocity / speed);
    (*velocities)[i] = velocity;
  }
  return total_contacts;
}

}  // namespace rx::physics::detail
