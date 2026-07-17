#include "placement_demo_assets.h"

#include <algorithm>
#include <cmath>

#include "core/math.h"

namespace rx {
namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kTau = 6.28318530717959f;

// Small deterministic integer hash / PRNG so every "random" detail (rock lumps,
// grass lean) is reproducible from a seed with no <random> global state.
u32 HashU32(u32 x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// Hashed float in [0, 1).
f32 Hash01(u32 x) { return static_cast<f32>(HashU32(x) & 0xffffffu) / 16777216.0f; }

// Hashed float in [-1, 1).
f32 Hash11(u32 x) { return Hash01(x) * 2.0f - 1.0f; }

// A vertex with a position and normal; uv/tangent are filled with a simple
// planar default (these demo meshes are flat-shaded low-poly, materials do the
// look). Returns the pushed index.
u32 PushVertex(asset::MeshLod* lod, const Vec3& p, const Vec3& n) {
  asset::Vertex v{};
  v.position[0] = p.x;
  v.position[1] = p.y;
  v.position[2] = p.z;
  v.normal[0] = n.x;
  v.normal[1] = n.y;
  v.normal[2] = n.z;
  // A stable tangent roughly perpendicular to the normal keeps the tangent
  // frame finite for normal-mapped materials; exact orientation is unimportant.
  Vec3 t = std::abs(n.y) < 0.99f ? Normalize(Cross(Vec3{0, 1, 0}, n)) : Vec3{1, 0, 0};
  v.tangent[0] = t.x;
  v.tangent[1] = t.y;
  v.tangent[2] = t.z;
  v.tangent[3] = 1.0f;
  v.uv[0] = 0.0f;
  v.uv[1] = 0.0f;
  u32 index = static_cast<u32>(lod->vertices.size());
  lod->vertices.push_back(v);
  return index;
}

// An orthonormal basis (t, b) spanning the plane perpendicular to axis `d`.
void Basis(const Vec3& d, Vec3* t, Vec3* b) {
  Vec3 up = std::abs(d.y) < 0.99f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
  *t = Normalize(Cross(up, d));
  *b = Cross(d, *t);
}

// A tapered tube (truncated cone) from p0 (radius r0) to p1 (radius r1) with
// `sides` radial segments, smooth radial normals. Two rings, wrap-around
// indexing so no seam vertices are needed.
void AddTube(asset::MeshLod* lod, const Vec3& p0, f32 r0, const Vec3& p1, f32 r1, u32 sides) {
  if (sides < 3) sides = 3;
  Vec3 axis = Normalize(p1 - p0);
  Vec3 t, b;
  Basis(axis, &t, &b);
  u32 base = static_cast<u32>(lod->vertices.size());
  for (u32 k = 0; k < sides; ++k) {
    f32 a = kTau * static_cast<f32>(k) / static_cast<f32>(sides);
    Vec3 radial = t * std::cos(a) + b * std::sin(a);
    PushVertex(lod, p0 + radial * r0, radial);
    PushVertex(lod, p1 + radial * r1, radial);
  }
  for (u32 k = 0; k < sides; ++k) {
    u32 k0 = base + k * 2;
    u32 k1 = base + ((k + 1) % sides) * 2;
    // lower ring = k0/k1, upper ring = k0+1/k1+1
    for (u32 i : {k0, k1, k0 + 1, k0 + 1, k1, k1 + 1}) lod->indices.push_back(i);
  }
}

// A cone with its base ring at `base_center` and apex `height` above it, flat
// per-face normals for a faceted low-poly canopy. Base cap is omitted (hidden
// by the geometry stacked below it).
void AddCone(asset::MeshLod* lod, const Vec3& base_center, f32 radius, f32 height, u32 sides) {
  if (sides < 3) sides = 3;
  Vec3 apex = base_center + Vec3{0, height, 0};
  for (u32 k = 0; k < sides; ++k) {
    f32 a0 = kTau * static_cast<f32>(k) / static_cast<f32>(sides);
    f32 a1 = kTau * static_cast<f32>(k + 1) / static_cast<f32>(sides);
    Vec3 p0 = base_center + Vec3{std::cos(a0) * radius, 0, std::sin(a0) * radius};
    Vec3 p1 = base_center + Vec3{std::cos(a1) * radius, 0, std::sin(a1) * radius};
    Vec3 n = Normalize(Cross(p1 - p0, apex - p0));
    u32 i0 = PushVertex(lod, p0, n);
    u32 i1 = PushVertex(lod, p1, n);
    u32 i2 = PushVertex(lod, apex, n);
    for (u32 i : {i0, i1, i2}) lod->indices.push_back(i);
  }
}

// A squashed uv-sphere (ellipsoid) centered at `center` with per-axis radii
// `scale`, smooth normals corrected for the non-uniform scale. Seam vertices
// are duplicated (segments + 1 columns) for clean wrapping.
void AddEllipsoid(asset::MeshLod* lod, const Vec3& center, const Vec3& scale, u32 rings,
                  u32 segments) {
  if (rings < 2) rings = 2;
  if (segments < 3) segments = 3;
  u32 base = static_cast<u32>(lod->vertices.size());
  for (u32 y = 0; y <= rings; ++y) {
    f32 phi = kPi * static_cast<f32>(y) / static_cast<f32>(rings);
    f32 sp = std::sin(phi), cp = std::cos(phi);
    for (u32 x = 0; x <= segments; ++x) {
      f32 theta = kTau * static_cast<f32>(x) / static_cast<f32>(segments);
      Vec3 u{sp * std::cos(theta), cp, sp * std::sin(theta)};
      Vec3 p{center.x + u.x * scale.x, center.y + u.y * scale.y, center.z + u.z * scale.z};
      // Ellipsoid surface normal: unit-sphere normal divided by the scale.
      Vec3 n = Normalize(Vec3{u.x / scale.x, u.y / scale.y, u.z / scale.z});
      PushVertex(lod, p, n);
    }
  }
  u32 stride = segments + 1;
  for (u32 y = 0; y < rings; ++y) {
    for (u32 x = 0; x < segments; ++x) {
      u32 a = base + y * stride + x;
      u32 c = a + stride;
      for (u32 i : {a, c, a + 1, a + 1, c, c + 1}) lod->indices.push_back(i);
    }
  }
}

// A single blade: a leaning quad plus its flipped-winding twin so it is lit and
// visible from both faces. `dir` is the in-plane horizontal direction.
void AddBlade(asset::MeshLod* lod, const Vec3& base, const Vec3& dir, f32 width, f32 height,
              const Vec3& lean) {
  Vec3 u = Normalize(dir);
  Vec3 bl = base + u * (-width * 0.5f);
  Vec3 br = base + u * (width * 0.5f);
  Vec3 top = base + Vec3{0, height, 0} + lean;
  Vec3 tl = top + u * (-width * 0.25f);
  Vec3 tr = top + u * (width * 0.25f);
  Vec3 n = Normalize(Cross(u, tl - bl));
  // Front face.
  u32 f0 = PushVertex(lod, bl, n);
  u32 f1 = PushVertex(lod, br, n);
  u32 f2 = PushVertex(lod, tr, n);
  u32 f3 = PushVertex(lod, tl, n);
  for (u32 i : {f0, f1, f2, f0, f2, f3}) lod->indices.push_back(i);
  // Back face: same corners, reversed winding, opposite normal.
  Vec3 nb = n * -1.0f;
  u32 b0 = PushVertex(lod, bl, nb);
  u32 b1 = PushVertex(lod, tl, nb);
  u32 b2 = PushVertex(lod, tr, nb);
  u32 b3 = PushVertex(lod, br, nb);
  for (u32 i : {b0, b1, b2, b0, b2, b3}) lod->indices.push_back(i);
}

// Face-average vertex normals over an index/vertex range (for meshes whose
// vertices were displaced after construction, e.g. the rock).
void RecomputeNormals(asset::MeshLod* lod, u32 vbegin, u32 vend, u32 ibegin, u32 iend) {
  for (u32 v = vbegin; v < vend; ++v) {
    lod->vertices[v].normal[0] = 0;
    lod->vertices[v].normal[1] = 0;
    lod->vertices[v].normal[2] = 0;
  }
  for (u32 i = ibegin; i + 2 < iend; i += 3) {
    u32 ia = lod->indices[i], ib = lod->indices[i + 1], ic = lod->indices[i + 2];
    const asset::Vertex& va = lod->vertices[ia];
    const asset::Vertex& vb = lod->vertices[ib];
    const asset::Vertex& vc = lod->vertices[ic];
    Vec3 pa{va.position[0], va.position[1], va.position[2]};
    Vec3 pb{vb.position[0], vb.position[1], vb.position[2]};
    Vec3 pc{vc.position[0], vc.position[1], vc.position[2]};
    Vec3 fn = Cross(pb - pa, pc - pa);  // area-weighted face normal
    for (u32 idx : {ia, ib, ic}) {
      lod->vertices[idx].normal[0] += fn.x;
      lod->vertices[idx].normal[1] += fn.y;
      lod->vertices[idx].normal[2] += fn.z;
    }
  }
  for (u32 v = vbegin; v < vend; ++v) {
    Vec3 n = Normalize(Vec3{lod->vertices[v].normal[0], lod->vertices[v].normal[1],
                            lod->vertices[v].normal[2]});
    if (Length(n) < 0.5f) n = Vec3{0, 1, 0};
    lod->vertices[v].normal[0] = n.x;
    lod->vertices[v].normal[1] = n.y;
    lod->vertices[v].normal[2] = n.z;
  }
}

// Append a submesh covering [begin, current end) with the given material hash.
void AddSubmesh(asset::MeshLod* lod, u32 begin, u64 material) {
  asset::Submesh s;
  s.index_offset = begin;
  s.index_count = static_cast<u32>(lod->indices.size()) - begin;
  s.material = asset::AssetId{material};
  lod->submeshes.push_back(s);
}

// Fill mesh.id and a tight bounding sphere from lod 0 (matching how
// primitives.cc reports bounds_center / bounds_radius).
void Finalize(asset::Mesh* mesh, asset::AssetId id) {
  mesh->id = id;
  const asset::MeshLod& lod = mesh->lods[0];
  Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  for (const asset::Vertex& v : lod.vertices) {
    lo.x = std::min(lo.x, v.position[0]);
    lo.y = std::min(lo.y, v.position[1]);
    lo.z = std::min(lo.z, v.position[2]);
    hi.x = std::max(hi.x, v.position[0]);
    hi.y = std::max(hi.y, v.position[1]);
    hi.z = std::max(hi.z, v.position[2]);
  }
  Vec3 center = (lo + hi) * 0.5f;
  f32 r = 0;
  for (const asset::Vertex& v : lod.vertices) {
    Vec3 p{v.position[0], v.position[1], v.position[2]};
    r = std::max(r, Length(p - center));
  }
  mesh->bounds_center[0] = center.x;
  mesh->bounds_center[1] = center.y;
  mesh->bounds_center[2] = center.z;
  mesh->bounds_radius = r;
}

}  // namespace

asset::Mesh MakePineTree(f32 height, u64 trunk_material, u64 canopy_material, asset::AssetId id) {
  asset::Mesh mesh;
  asset::MeshLod& lod = mesh.lods.emplace_back();

  const f32 trunk_h = height * 0.28f;
  const f32 trunk_r = height * 0.045f;

  u32 trunk_begin = static_cast<u32>(lod.indices.size());
  AddTube(&lod, Vec3{0, 0, 0}, trunk_r, Vec3{0, trunk_h, 0}, trunk_r * 0.7f, 7);
  AddSubmesh(&lod, trunk_begin, trunk_material);

  // Three stacked cones of shrinking radius covering the upper trunk to the tip.
  u32 canopy_begin = static_cast<u32>(lod.indices.size());
  const f32 canopy_bottom = trunk_h * 0.7f;
  const f32 canopy_span = height - canopy_bottom;
  const int kCones = 3;
  for (int c = 0; c < kCones; ++c) {
    f32 f = static_cast<f32>(c) / kCones;
    f32 y = canopy_bottom + canopy_span * f;
    f32 radius = height * 0.26f * (1.0f - 0.55f * f);
    f32 cone_h = canopy_span * 0.55f;
    AddCone(&lod, Vec3{0, y, 0}, radius, cone_h, 8);
  }
  AddSubmesh(&lod, canopy_begin, canopy_material);

  Finalize(&mesh, id);
  return mesh;
}

asset::Mesh MakeBroadleafTree(f32 height, u64 trunk_material, u64 canopy_material,
                              asset::AssetId id) {
  asset::Mesh mesh;
  asset::MeshLod& lod = mesh.lods.emplace_back();

  const f32 trunk_h = height * 0.5f;
  const f32 trunk_r = height * 0.05f;

  u32 trunk_begin = static_cast<u32>(lod.indices.size());
  AddTube(&lod, Vec3{0, 0, 0}, trunk_r, Vec3{0, trunk_h, 0}, trunk_r * 0.75f, 7);
  AddSubmesh(&lod, trunk_begin, trunk_material);

  // Three overlapping squashed spheres forming a rounded crown.
  u32 canopy_begin = static_cast<u32>(lod.indices.size());
  const f32 crown_r = height * 0.32f;
  const f32 crown_y = trunk_h + crown_r * 0.6f;
  struct Blob {
    Vec3 offset;
    f32 scale;
  };
  const Blob blobs[] = {
      {{0.0f, 0.0f, 0.0f}, 1.0f},
      {{crown_r * 0.55f, -crown_r * 0.15f, crown_r * 0.1f}, 0.72f},
      {{-crown_r * 0.45f, crown_r * 0.05f, -crown_r * 0.35f}, 0.66f},
  };
  for (const Blob& b : blobs) {
    Vec3 c{b.offset.x, crown_y + b.offset.y, b.offset.z};
    Vec3 s{crown_r * b.scale, crown_r * 0.82f * b.scale, crown_r * b.scale};
    AddEllipsoid(&lod, c, s, 5, 7);
  }
  AddSubmesh(&lod, canopy_begin, canopy_material);

  Finalize(&mesh, id);
  return mesh;
}

asset::Mesh MakeBush(f32 radius, u64 material, asset::AssetId id) {
  asset::Mesh mesh;
  asset::MeshLod& lod = mesh.lods.emplace_back();

  u32 begin = static_cast<u32>(lod.indices.size());
  // Three low overlapping squashed spheres resting on the ground.
  struct Blob {
    Vec3 offset;
    f32 scale;
  };
  const Blob blobs[] = {
      {{0.0f, 0.0f, 0.0f}, 1.0f},
      {{radius * 0.6f, -radius * 0.1f, radius * 0.15f}, 0.7f},
      {{-radius * 0.5f, -radius * 0.05f, -radius * 0.4f}, 0.62f},
  };
  for (const Blob& b : blobs) {
    // Sit on the ground: center raised by the squashed vertical radius.
    f32 vr = radius * 0.6f * b.scale;
    Vec3 c{b.offset.x, vr + b.offset.y, b.offset.z};
    Vec3 s{radius * b.scale, vr, radius * b.scale};
    AddEllipsoid(&lod, c, s, 5, 7);
  }
  AddSubmesh(&lod, begin, material);

  Finalize(&mesh, id);
  return mesh;
}

asset::Mesh MakeRock(f32 radius, u32 seed, u64 material, asset::AssetId id) {
  asset::Mesh mesh;
  asset::MeshLod& lod = mesh.lods.emplace_back();

  const u32 rings = 6, segments = 9;
  const u32 vbegin = 0, ibegin = 0;
  // Fixed lump amounts for the two poles so their fan of duplicate columns does
  // not split apart.
  const f32 pole_top = 1.0f + 0.28f * Hash11(seed * 2654435761u + 11u);
  const f32 pole_bot = 1.0f + 0.28f * Hash11(seed * 2654435761u + 97u);

  for (u32 y = 0; y <= rings; ++y) {
    f32 phi = kPi * static_cast<f32>(y) / static_cast<f32>(rings);
    f32 sp = std::sin(phi), cp = std::cos(phi);
    for (u32 x = 0; x <= segments; ++x) {
      f32 theta = kTau * static_cast<f32>(x) / static_cast<f32>(segments);
      Vec3 u{sp * std::cos(theta), cp, sp * std::sin(theta)};
      f32 lump;
      if (y == 0) {
        lump = pole_top;
      } else if (y == rings) {
        lump = pole_bot;
      } else {
        u32 xx = x % segments;  // seam column shares the first column's lump
        lump = 1.0f + 0.28f * Hash11(seed * 2654435761u + y * 9781u + xx * 668265263u);
      }
      Vec3 p{u.x * radius * lump, u.y * radius * lump * 0.7f, u.z * radius * lump};
      PushVertex(&lod, p, u);  // normal recomputed below
    }
  }
  u32 stride = segments + 1;
  for (u32 y = 0; y < rings; ++y) {
    for (u32 x = 0; x < segments; ++x) {
      u32 a = y * stride + x;
      u32 c = a + stride;
      for (u32 i : {a, c, a + 1, a + 1, c, c + 1}) lod.indices.push_back(i);
    }
  }
  // Lift so the lowest point sits at y = 0 (rests on the ground).
  f32 min_y = 1e30f;
  for (const asset::Vertex& v : lod.vertices) min_y = std::min(min_y, v.position[1]);
  for (asset::Vertex& v : lod.vertices) v.position[1] -= min_y;

  RecomputeNormals(&lod, vbegin, static_cast<u32>(lod.vertices.size()), ibegin,
                   static_cast<u32>(lod.indices.size()));
  AddSubmesh(&lod, 0, material);

  Finalize(&mesh, id);
  return mesh;
}

asset::Mesh MakeGrassTuft(f32 height, u64 material, asset::AssetId id) {
  asset::Mesh mesh;
  asset::MeshLod& lod = mesh.lods.emplace_back();

  u32 begin = static_cast<u32>(lod.indices.size());
  const u32 kBlades = 4;
  const f32 width = height * 0.35f;
  for (u32 i = 0; i < kBlades; ++i) {
    // Deterministic per-blade angle, height and lean.
    f32 angle = kTau * (static_cast<f32>(i) / kBlades) + Hash11(i * 31u + 1u) * 0.4f;
    Vec3 dir{std::cos(angle), 0, std::sin(angle)};
    Vec3 base{Hash11(i * 7u + 3u) * width * 0.3f, 0, Hash11(i * 13u + 5u) * width * 0.3f};
    f32 h = height * (0.7f + 0.3f * Hash01(i * 17u + 9u));
    Vec3 lean{Hash11(i * 23u + 2u) * height * 0.25f, 0, Hash11(i * 29u + 4u) * height * 0.25f};
    AddBlade(&lod, base, dir, width, h, lean);
  }
  AddSubmesh(&lod, begin, material);

  // Dense fill geometry stays out of the ray tracing acceleration structures.
  mesh.exclude_from_rt = true;
  Finalize(&mesh, id);
  return mesh;
}

asset::Mesh MakeDeadTree(f32 height, u64 material, asset::AssetId id) {
  asset::Mesh mesh;
  asset::MeshLod& lod = mesh.lods.emplace_back();

  u32 begin = static_cast<u32>(lod.indices.size());
  const f32 trunk_r = height * 0.05f;
  const f32 trunk_top = height * 0.85f;
  AddTube(&lod, Vec3{0, 0, 0}, trunk_r, Vec3{0, trunk_top, 0}, trunk_r * 0.4f, 6);

  // Three tapered branch stubs angling up and outward from the upper trunk.
  const int kBranches = 3;
  for (int b = 0; b < kBranches; ++b) {
    f32 f = static_cast<f32>(b) / kBranches;
    f32 y = height * (0.5f + 0.32f * f);
    f32 angle = kTau * f + Hash11(static_cast<u32>(b) * 41u + 7u) * 0.5f;
    Vec3 dir = Normalize(Vec3{std::cos(angle), 0.8f, std::sin(angle)});
    f32 len = height * (0.28f - 0.06f * f);
    Vec3 root{0, y, 0};
    f32 r = trunk_r * (0.55f - 0.12f * f);
    AddTube(&lod, root, r, root + dir * len, r * 0.25f, 5);
  }
  AddSubmesh(&lod, begin, material);

  Finalize(&mesh, id);
  return mesh;
}

}  // namespace rx
