#include "asset/primitives.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/math.h"

namespace rx::asset {
namespace {

// One coarser lod by vertex clustering: snap each vertex to a g x g x g grid
// cell over the mesh bounds, average the vertices that land in a cell into one
// representative, and keep only the triangles whose three corners fall in three
// distinct cells (the rest have folded up). Lower quality than edge collapse but
// robust and fine for the distant lods the selector reaches for.
MeshLod ClusterDecimate(const MeshLod& src, const Vec3& bmin, const Vec3& ext, u32 g) {
  f32 cell[3] = {std::max(ext.x, 1e-5f) / g, std::max(ext.y, 1e-5f) / g,
                 std::max(ext.z, 1e-5f) / g};
  auto cell_of = [&](const Vertex& v) -> u64 {
    u32 cx = std::min(static_cast<u32>((v.position[0] - bmin.x) / cell[0]), g - 1);
    u32 cy = std::min(static_cast<u32>((v.position[1] - bmin.y) / cell[1]), g - 1);
    u32 cz = std::min(static_cast<u32>((v.position[2] - bmin.z) / cell[2]), g - 1);
    return (static_cast<u64>(cz) * g + cy) * g + cx;
  };

  struct Accum {
    f64 p[3] = {0, 0, 0};
    f64 n[3] = {0, 0, 0};
    f64 t[3] = {0, 0, 0};
    f64 uv[2] = {0, 0};
    u32 count = 0;
    u32 color = 0xffffffff;
  };
  std::unordered_map<u64, u32> cell_to_new;
  base::Vector<Accum> accum;
  base::Vector<u32> remap(src.vertices.size());
  for (size_t i = 0; i < src.vertices.size(); ++i) {
    const Vertex& v = src.vertices[i];
    u64 c = cell_of(v);
    auto it = cell_to_new.find(c);
    u32 ni;
    if (it == cell_to_new.end()) {
      ni = static_cast<u32>(accum.size());
      cell_to_new.emplace(c, ni);
      Accum a;
      a.color = v.color;
      accum.push_back(a);
    } else {
      ni = it->second;
    }
    remap[i] = ni;
    Accum& a = accum[ni];
    for (int k = 0; k < 3; ++k) {
      a.p[k] += v.position[k];
      a.n[k] += v.normal[k];
      a.t[k] += v.tangent[k];
    }
    a.uv[0] += v.uv[0];
    a.uv[1] += v.uv[1];
    ++a.count;
  }

  MeshLod out;
  out.vertices.reserve(accum.size());
  for (const Accum& a : accum) {
    Vertex v{};
    f64 inv = a.count ? 1.0 / a.count : 1.0;
    for (int k = 0; k < 3; ++k) v.position[k] = static_cast<f32>(a.p[k] * inv);
    Vec3 n = Normalize(
        Vec3{static_cast<f32>(a.n[0]), static_cast<f32>(a.n[1]), static_cast<f32>(a.n[2])});
    v.normal[0] = n.x;
    v.normal[1] = n.y;
    v.normal[2] = n.z;
    Vec3 t = Normalize(
        Vec3{static_cast<f32>(a.t[0]), static_cast<f32>(a.t[1]), static_cast<f32>(a.t[2])});
    v.tangent[0] = t.x;
    v.tangent[1] = t.y;
    v.tangent[2] = t.z;
    v.tangent[3] = 1.0f;
    v.uv[0] = static_cast<f32>(a.uv[0] * inv);
    v.uv[1] = static_cast<f32>(a.uv[1] * inv);
    v.color = a.color;
    out.vertices.push_back(v);
  }
  for (size_t i = 0; i + 2 < src.indices.size(); i += 3) {
    u32 a = remap[src.indices[i]], b = remap[src.indices[i + 1]], c = remap[src.indices[i + 2]];
    if (a != b && b != c && a != c) {
      out.indices.push_back(a);
      out.indices.push_back(b);
      out.indices.push_back(c);
    }
  }
  AssetId material = src.submeshes.empty() ? AssetId{} : src.submeshes[0].material;
  out.submeshes.push_back({0, static_cast<u32>(out.indices.size()), material});
  return out;
}

// One bone of the test biped: name, parent index, offset from the parent joint
// in engine space. Identity bind rotation keeps local axes world-aligned so the
// locomotion's about-X swings read as forward/back.
enum Cap { kNone, kHead, kFoot, kHand };
struct BipedBone {
  const char* name;
  i32 parent;
  Vec3 offset;
  Cap cap;  // leaf cap box extending past the joint
};

// pelvis ~1m up; legs down -Y, arms out +/-X, spine up +Y.
const BipedBone kBiped[] = {
    {"NPC Root [Root]", -1, {0, 0, 0}, kNone},
    {"NPC Pelvis [Pelv]", 0, {0, 1.0f, 0}, kNone},
    {"NPC Spine [Spn0]", 1, {0, 0.18f, 0}, kNone},
    {"NPC Spine1 [Spn1]", 2, {0, 0.18f, 0}, kNone},
    {"NPC Spine2 [Spn2]", 3, {0, 0.20f, 0}, kNone},
    {"NPC Head [Head]", 4, {0, 0.22f, 0}, kHead},
    {"NPC L Thigh [LThg]", 1, {0.11f, -0.04f, 0}, kNone},
    {"NPC L Calf [LClf]", 6, {0, -0.44f, 0}, kNone},
    {"NPC L Foot [Lft ]", 7, {0, -0.44f, 0}, kFoot},
    {"NPC R Thigh [RThg]", 1, {-0.11f, -0.04f, 0}, kNone},
    {"NPC R Calf [RClf]", 9, {0, -0.44f, 0}, kNone},
    {"NPC R Foot [Rft ]", 10, {0, -0.44f, 0}, kFoot},
    {"NPC L UpperArm [LUar]", 4, {0.18f, 0.0f, 0}, kNone},
    {"NPC L Forearm [LLar]", 12, {0.28f, 0, 0}, kHand},
    {"NPC R UpperArm [RUar]", 4, {-0.18f, 0.0f, 0}, kNone},
    {"NPC R Forearm [RLar]", 14, {-0.28f, 0, 0}, kHand},
};

// Append an axis-aligned box spanning a..b inflated by `thick`, all vertices
// weighted fully to `bone`.
void AddBox(MeshLod* lod, const Vec3& a, const Vec3& b, f32 thick, u8 bone) {
  f32 lo[3] = {std::min(a.x, b.x) - thick, std::min(a.y, b.y) - thick, std::min(a.z, b.z) - thick};
  f32 hi[3] = {std::max(a.x, b.x) + thick, std::max(a.y, b.y) + thick, std::max(a.z, b.z) + thick};
  static constexpr int kFace[6][3] = {{0, 1, 2}, {0, 1, 2}, {1, 0, 2},
                                      {1, 0, 2}, {2, 0, 1}, {2, 0, 1}};
  static constexpr f32 kSign[6] = {1, -1, 1, -1, 1, -1};
  for (int f = 0; f < 6; ++f) {
    int n = kFace[f][0], u = kFace[f][1], v = kFace[f][2];
    f32 normal[3] = {0, 0, 0};
    normal[n] = kSign[f];
    u32 base = static_cast<u32>(lod->vertices.size());
    for (int c = 0; c < 4; ++c) {
      f32 cu = (c == 1 || c == 2) ? 1.0f : 0.0f;
      f32 cv = (c >= 2) ? 1.0f : 0.0f;
      Vertex vertex{};
      vertex.position[n] = kSign[f] > 0 ? hi[n] : lo[n];
      vertex.position[u] = cu > 0 ? hi[u] : lo[u];
      vertex.position[v] = cv > 0 ? hi[v] : lo[v];
      vertex.normal[n] = kSign[f];
      vertex.tangent[u] = 1.0f;
      vertex.tangent[3] = 1.0f;
      vertex.uv[0] = cu;
      vertex.uv[1] = cv;
      lod->vertices.push_back(vertex);
      SkinnedVertexExtra extra;
      extra.bone_indices[0] = bone;
      extra.bone_weights[0] = 255;
      lod->skinning.push_back(extra);
    }
    bool flip = kSign[f] < 0;
    if (flip) {
      for (u32 i : {0u, 2u, 1u, 0u, 3u, 2u}) lod->indices.push_back(base + i);
    } else {
      for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) lod->indices.push_back(base + i);
    }
  }
}

}  // namespace

Mesh MakeBox(f32 hx, f32 hy, f32 hz, AssetId id) {
  struct Face {
    f32 n[3];  // normal
    f32 u[3];  // tangent axes, cross(u, v) == n so corners wind ccw
    f32 v[3];
  };
  static constexpr Face kFaces[] = {
      {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
      {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},   {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},
  };
  static constexpr f32 kCorners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  static constexpr f32 kUvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  const f32 half[3] = {hx, hy, hz};

  Mesh mesh;
  mesh.id = id;
  MeshLod& lod = mesh.lods.emplace_back();
  lod.vertices.reserve(24);
  lod.indices.reserve(36);

  for (const Face& face : kFaces) {
    u32 base = static_cast<u32>(lod.vertices.size());
    for (int corner = 0; corner < 4; ++corner) {
      Vertex vertex{};
      for (int axis = 0; axis < 3; ++axis) {
        vertex.position[axis] = half[axis] * (face.n[axis] + kCorners[corner][0] * face.u[axis] +
                                              kCorners[corner][1] * face.v[axis]);
        vertex.normal[axis] = face.n[axis];
        vertex.tangent[axis] = face.u[axis];
      }
      vertex.tangent[3] = 1.0f;
      vertex.uv[0] = kUvs[corner][0];
      vertex.uv[1] = kUvs[corner][1];
      lod.vertices.push_back(vertex);
    }
    for (u32 index : {0u, 1u, 2u, 0u, 2u, 3u}) lod.indices.push_back(base + index);
  }

  mesh.bounds_radius = std::sqrt(hx * hx + hy * hy + hz * hz);
  return mesh;
}

Mesh MakeCube(f32 half_extent, AssetId id) {
  return MakeBox(half_extent, half_extent, half_extent, id);
}

Mesh MakeSphere(f32 radius, u32 rings, u32 segments, AssetId id) {
  Mesh mesh;
  mesh.id = id;
  MeshLod& lod = mesh.lods.emplace_back();
  rings = rings < 2 ? 2 : rings;
  segments = segments < 3 ? 3 : segments;

  for (u32 y = 0; y <= rings; ++y) {
    f32 v = static_cast<f32>(y) / static_cast<f32>(rings);
    f32 phi = v * 3.14159265f;  // 0..pi, pole to pole
    f32 sin_phi = std::sin(phi), cos_phi = std::cos(phi);
    for (u32 x = 0; x <= segments; ++x) {
      f32 u = static_cast<f32>(x) / static_cast<f32>(segments);
      f32 theta = u * 6.2831853f;
      f32 sin_theta = std::sin(theta), cos_theta = std::cos(theta);
      Vec3 n{sin_phi * cos_theta, cos_phi, sin_phi * sin_theta};
      Vertex vertex{};
      vertex.position[0] = n.x * radius;
      vertex.position[1] = n.y * radius;
      vertex.position[2] = n.z * radius;
      vertex.normal[0] = n.x;
      vertex.normal[1] = n.y;
      vertex.normal[2] = n.z;
      // Tangent runs along +theta (the latitude circle), for anisotropy.
      vertex.tangent[0] = -sin_theta;
      vertex.tangent[1] = 0.0f;
      vertex.tangent[2] = cos_theta;
      vertex.tangent[3] = 1.0f;
      vertex.uv[0] = u;
      vertex.uv[1] = v;
      lod.vertices.push_back(vertex);
    }
  }

  u32 stride = segments + 1;
  for (u32 y = 0; y < rings; ++y) {
    for (u32 x = 0; x < segments; ++x) {
      u32 a = y * stride + x;
      u32 b = a + stride;
      for (u32 i : {a, b, a + 1, a + 1, b, b + 1}) lod.indices.push_back(i);
    }
  }

  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), AssetId{}});
  mesh.bounds_radius = radius;
  return mesh;
}

Mesh MakeLodSphere(f32 radius, AssetId id) {
  Mesh mesh;
  mesh.id = id;
  // Fine, medium, coarse tessellation. Each MakeSphere produces a one-lod mesh;
  // move that lod into the shared list so the gpu picks it by distance.
  const u32 tess[][2] = {{48, 64}, {16, 22}, {6, 8}};
  for (const auto& t : tess) {
    Mesh level = MakeSphere(radius, t[0], t[1], id);
    mesh.lods.push_back(std::move(level.lods[0]));
  }
  mesh.bounds_radius = radius;
  return mesh;
}

void GenerateLods(Mesh* mesh) {
  if (mesh->skinned || mesh->lods.size() != 1) return;
  if (mesh->lods[0].submeshes.size() > 1) return;  // single material only
  if (mesh->lods[0].indices.size() < 3000) return;  // not worth lod'ing small meshes
  // Copy: push_back below reallocates mesh->lods, so we must not hold a reference
  // into it across the loop.
  const MeshLod base = mesh->lods[0];

  Vec3 bmin{1e30f, 1e30f, 1e30f}, bmax{-1e30f, -1e30f, -1e30f};
  for (const Vertex& v : base.vertices) {
    bmin.x = std::min(bmin.x, v.position[0]);
    bmin.y = std::min(bmin.y, v.position[1]);
    bmin.z = std::min(bmin.z, v.position[2]);
    bmax.x = std::max(bmax.x, v.position[0]);
    bmax.y = std::max(bmax.y, v.position[1]);
    bmax.z = std::max(bmax.z, v.position[2]);
  }
  Vec3 ext{bmax.x - bmin.x, bmax.y - bmin.y, bmax.z - bmin.z};

  const u32 grids[2] = {24, 9};  // medium then coarse cell counts per axis
  for (u32 g : grids) {
    MeshLod lod = ClusterDecimate(base, bmin, ext, g);
    if (lod.indices.size() >= 3 && lod.indices.size() < mesh->lods.back().indices.size()) {
      mesh->lods.push_back(std::move(lod));
    }
  }
}

void MakeSkinnedBiped(AssetId mesh_id, Skeleton* out_skeleton, Mesh* out_mesh) {
  u32 count = static_cast<u32>(sizeof(kBiped) / sizeof(kBiped[0]));

  // Skeleton: identity bind rotation, translation from the offset table.
  out_skeleton->bones.clear();
  base::Vector<Vec3> world(count);
  for (u32 i = 0; i < count; ++i) {
    Bone bone;
    bone.name = kBiped[i].name;
    bone.parent = kBiped[i].parent;
    bone.bind_translation = kBiped[i].offset;
    bone.bind_rotation = {0, 0, 0, 1};
    bone.bind_scale = 1.0f;
    out_skeleton->bones.push_back(std::move(bone));
    world[i] = kBiped[i].parent >= 0 ? world[kBiped[i].parent] + kBiped[i].offset
                                     : kBiped[i].offset;
  }

  // Skin binding: one entry per bone (skin-local index == skeleton index), the
  // inverse bind is the inverse of the identity-rotation bind world.
  *out_mesh = Mesh{};
  out_mesh->id = mesh_id;
  out_mesh->skinned = true;
  for (u32 i = 0; i < count; ++i) {
    out_mesh->skin.bones.push_back(kBiped[i].name);
    out_mesh->skin.inverse_bind.push_back(MakeTranslation({-world[i].x, -world[i].y, -world[i].z}));
  }

  MeshLod& lod = out_mesh->lods.emplace_back();
  for (u32 i = 0; i < count; ++i) {
    // Limb box from the parent joint to this joint, weighted to the parent.
    if (kBiped[i].parent >= 0) {
      f32 thick = i >= 6 ? 0.06f : 0.09f;  // limbs thinner than the torso
      AddBox(&lod, world[kBiped[i].parent], world[i], thick,
             static_cast<u8>(kBiped[i].parent));
    }
    // Cap boxes for leaves.
    Vec3 tip = world[i];
    if (kBiped[i].cap == kHead) {
      AddBox(&lod, tip, tip + Vec3{0, 0.12f, 0}, 0.12f, static_cast<u8>(i));
    } else if (kBiped[i].cap == kFoot) {
      AddBox(&lod, tip, tip + Vec3{0, -0.02f, 0.18f}, 0.06f, static_cast<u8>(i));
    } else if (kBiped[i].cap == kHand) {
      Vec3 dir = world[i] - world[kBiped[i].parent];
      AddBox(&lod, tip, tip + Vec3{dir.x > 0 ? 0.08f : -0.08f, 0, 0}, 0.05f, static_cast<u8>(i));
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), AssetId{}});

  out_mesh->bounds_center[1] = 1.0f;
  out_mesh->bounds_radius = 2.0f;
  out_mesh->exclude_from_rt = true;  // dynamic skinned geometry stays out of the tlas
}

}  // namespace rx::asset
