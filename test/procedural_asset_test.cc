// Procedural asset generation: the pattern texture synthesis and the blockout
// primitives a .rxscene may ask for. Pure CPU, no device.
#include <cmath>
#include <cstdio>
#include <string>

#include "asset/primitives.h"
#include "asset/procedural_texture.h"

namespace asset = rx::asset;
using rx::f32;
using rx::u32;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      std::fprintf(stderr, "procedural_asset_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                                                  \
    }                                                                                              \
  } while (0)

void TestPatternNames() {
  asset::PatternKind kind = asset::PatternKind::kNoise;
  CHECK(asset::ParsePatternKind("checker", &kind) && kind == asset::PatternKind::kChecker);
  CHECK(asset::ParsePatternKind("brick", &kind) && kind == asset::PatternKind::kBrick);
  // An unknown name must be rejected rather than resolving to a default: the
  // scene loader turns the false into a failed load naming the offending kind.
  CHECK(!asset::ParsePatternKind("plaid", &kind));
  CHECK(!asset::ParsePatternKind("Checker", &kind));
  CHECK(kind == asset::PatternKind::kBrick);  // left untouched by the failures
}

void TestCheckerMask() {
  asset::PatternDesc desc;
  desc.kind = asset::PatternKind::kChecker;
  desc.scale[0] = desc.scale[1] = 4.0f;  // 4 cells across, so cell centres sit at 1/8, 3/8, ...
  const f32 low = asset::SamplePattern(desc, 0.125f, 0.125f);
  const f32 high = asset::SamplePattern(desc, 0.375f, 0.125f);
  CHECK(low < 0.01f);
  CHECK(high > 0.99f);
  // Diagonal neighbours share a colour, orthogonal ones do not.
  CHECK(std::abs(asset::SamplePattern(desc, 0.375f, 0.375f) - low) < 0.01f);
  // Tiles: the mask at u and at u + 1 is the same field.
  CHECK(std::abs(asset::SamplePattern(desc, 1.125f, 0.125f) - low) < 0.01f);
}

// The two axes have to be independent, because the thing they describe is: a
// facade is bays across by floors up, and a tower is not a cube. With one
// scalar the cells took the face's aspect ratio instead of the author's word.
void TestPatternAxes() {
  asset::PatternDesc desc;
  desc.kind = asset::PatternKind::kGrid;
  desc.line_width = 0.2f;
  // Two cells across, eight up: cell centres at u = 1/4, 3/4 and v = 1/16,
  // 3/16, ... so a step of one v cell lands on a line while the same step
  // across u stays well inside a cell.
  desc.scale[0] = 2.0f;
  desc.scale[1] = 8.0f;
  CHECK(asset::SamplePattern(desc, 0.25f, 0.0625f) > 0.99f);   // cell centre
  CHECK(asset::SamplePattern(desc, 0.25f, 0.125f) < 0.01f);    // line one cell up
  CHECK(asset::SamplePattern(desc, 0.3125f, 0.0625f) > 0.99f); // same fraction across, still inside

  // Swapping the axes has to swap the result, or they are not independent.
  desc.scale[0] = 8.0f;
  desc.scale[1] = 2.0f;
  CHECK(asset::SamplePattern(desc, 0.0625f, 0.25f) > 0.99f);
  CHECK(asset::SamplePattern(desc, 0.125f, 0.25f) < 0.01f);

  // Noise wraps on each axis with that axis's period, so a field stretched one
  // way still tiles: the uv seam a sphere has would show it if it did not.
  desc.kind = asset::PatternKind::kNoise;
  desc.scale[0] = 3.0f;
  desc.scale[1] = 12.0f;
  for (u32 i = 0; i < 8; ++i) {
    const f32 u = i / 8.0f, v = i / 5.0f;
    CHECK(std::abs(asset::SamplePattern(desc, u + 1.0f, v) -
                   asset::SamplePattern(desc, u, v)) < 1e-4f);
    CHECK(std::abs(asset::SamplePattern(desc, u, v + 1.0f) -
                   asset::SamplePattern(desc, u, v)) < 1e-4f);
  }
}

void TestPatternRange() {
  // Every pattern has to stay in 0..1 whatever it is fed, because the bakes
  // interpolate colour and roughness endpoints with it unclamped.
  const asset::PatternKind kinds[] = {asset::PatternKind::kChecker, asset::PatternKind::kGrid,
                                      asset::PatternKind::kBrick, asset::PatternKind::kGradient,
                                      asset::PatternKind::kNoise};
  for (asset::PatternKind kind : kinds) {
    asset::PatternDesc desc;
    desc.kind = kind;
    desc.scale[0] = desc.scale[1] = 5.0f;
    for (u32 y = 0; y < 32; ++y) {
      for (u32 x = 0; x < 32; ++x) {
        f32 mask = asset::SamplePattern(desc, x / 31.0f, y / 31.0f);
        CHECK(mask >= 0.0f && mask <= 1.0f);
      }
    }
  }
}

void TestColorTexture() {
  asset::PatternDesc desc;
  desc.kind = asset::PatternKind::kChecker;
  desc.width = 64;
  desc.height = 32;
  desc.scale[0] = desc.scale[1] = 4.0f;
  const f32 black[3] = {0, 0, 0};
  const f32 white[3] = {1, 1, 1};
  asset::Texture texture =
      asset::MakePatternTexture(desc, black, white, /*srgb=*/true, asset::MakeAssetId("t"));
  CHECK(texture.format == asset::TextureFormat::kRgba8);
  CHECK(texture.width == 64 && texture.height == 32);
  CHECK(texture.is_srgb);
  CHECK(texture.data.size() == 64u * 32u * 4u);

  bool saw_dark = false, saw_light = false;
  for (size_t i = 0; i < texture.data.size(); i += 4) {
    saw_dark |= texture.data[i] < 8;
    saw_light |= texture.data[i] > 247;
    CHECK(texture.data[i + 3] == 255);
  }
  CHECK(saw_dark && saw_light);

  // The same mask baked linear must encode a mid grey darker than the sRGB one,
  // which is the whole reason is_srgb has to follow the slot.
  const f32 grey[3] = {0.5f, 0.5f, 0.5f};
  asset::Texture linear =
      asset::MakePatternTexture(desc, grey, grey, /*srgb=*/false, asset::MakeAssetId("t2"));
  asset::Texture encoded =
      asset::MakePatternTexture(desc, grey, grey, /*srgb=*/true, asset::MakeAssetId("t3"));
  CHECK(!linear.is_srgb);
  CHECK(linear.data[0] == 128);
  CHECK(encoded.data[0] > 180);
}

void TestNormalMap() {
  asset::PatternDesc desc;
  desc.kind = asset::PatternKind::kBrick;
  desc.width = 64;
  desc.height = 64;
  desc.scale[0] = desc.scale[1] = 4.0f;

  asset::Texture flat = asset::MakePatternNormalMap(desc, 0.0f, asset::MakeAssetId("n0"));
  CHECK(!flat.is_srgb);
  for (size_t i = 0; i < flat.data.size(); i += 4) {
    CHECK(flat.data[i] == 128 && flat.data[i + 1] == 128 && flat.data[i + 2] == 255);
  }

  asset::Texture relief = asset::MakePatternNormalMap(desc, 0.05f, asset::MakeAssetId("n1"));
  bool tilted = false;
  for (size_t i = 0; i < relief.data.size(); i += 4) {
    f32 n[3];
    for (int c = 0; c < 3; ++c) n[c] = relief.data[i + c] / 255.0f * 2.0f - 1.0f;
    CHECK(std::abs(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0f) < 0.02f);
    CHECK(n[2] > 0.0f);  // a heightfield normal never points into the surface
    tilted |= std::abs(n[0]) > 0.2f || std::abs(n[1]) > 0.2f;
  }
  CHECK(tilted);
}

void TestRoughnessMap() {
  asset::PatternDesc desc;
  desc.kind = asset::PatternKind::kChecker;
  desc.width = 32;
  desc.height = 32;
  desc.scale[0] = desc.scale[1] = 4.0f;
  asset::Texture mr = asset::MakePatternRoughnessMap(desc, 0.0f, 1.0f, asset::MakeAssetId("r"));
  CHECK(!mr.is_srgb);
  bool saw_smooth = false, saw_rough = false;
  for (size_t i = 0; i < mr.data.size(); i += 4) {
    saw_smooth |= mr.data[i + 1] < 8;
    saw_rough |= mr.data[i + 1] > 247;
    // Blue stays saturated so the material's metallic factor passes through.
    CHECK(mr.data[i + 2] == 255);
  }
  CHECK(saw_smooth && saw_rough);
}

// Shared invariants: indices in range, unit normals, tangents perpendicular to
// them, uvs on the unit square and one submesh covering every index.
void CheckMesh(const char* what, const asset::Mesh& mesh) {
  CHECK(mesh.lods.size() == 1);
  if (mesh.lods.empty()) return;
  const asset::MeshLod& lod = mesh.lods[0];
  CHECK(!lod.vertices.empty());
  CHECK(lod.indices.size() % 3 == 0);
  CHECK(lod.submeshes.size() == 1);
  if (lod.submeshes.size() == 1) {
    CHECK(lod.submeshes[0].index_offset == 0);
    CHECK(lod.submeshes[0].index_count == lod.indices.size());
  }
  for (u32 index : lod.indices) CHECK(index < lod.vertices.size());

  for (const asset::Vertex& v : lod.vertices) {
    f32 len = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] +
                        v.normal[2] * v.normal[2]);
    if (std::abs(len - 1.0f) > 1e-3f) {
      std::fprintf(stderr, "procedural_asset_test: %s has a non-unit normal (%f)\n", what, len);
      ++failures;
      break;
    }
    f32 dot = v.normal[0] * v.tangent[0] + v.normal[1] * v.tangent[1] + v.normal[2] * v.tangent[2];
    if (std::abs(dot) > 1e-3f) {
      std::fprintf(stderr, "procedural_asset_test: %s tangent not perpendicular (%f)\n", what, dot);
      ++failures;
      break;
    }
    if (v.uv[0] < -1e-4f || v.uv[0] > 1.0001f || v.uv[1] < -1e-4f || v.uv[1] > 1.0001f) {
      std::fprintf(stderr, "procedural_asset_test: %s uv off the unit square (%f %f)\n", what,
                   v.uv[0], v.uv[1]);
      ++failures;
      break;
    }
  }
}

void TestPrimitives() {
  const asset::AssetId id = asset::MakeAssetId("p");
  CheckMesh("plane", asset::MakePlane(2.0f, 3.0f, id));
  CheckMesh("cylinder", asset::MakeCylinder(0.5f, 1.0f, 24, id));
  CheckMesh("cone", asset::MakeCone(0.5f, 1.0f, 24, id));
  CheckMesh("torus", asset::MakeTorus(1.0f, 0.25f, 20, 32, id));
  CheckMesh("capsule", asset::MakeCapsule(0.4f, 0.6f, 16, 24, id));

  // The plane faces +Y and spans its half extents.
  asset::Mesh plane = asset::MakePlane(2.0f, 3.0f, id);
  for (const asset::Vertex& v : plane.lods[0].vertices) {
    CHECK(v.normal[1] > 0.999f);
    CHECK(std::abs(std::abs(v.position[0]) - 2.0f) < 1e-5f);
    CHECK(std::abs(v.position[1]) < 1e-5f);
    CHECK(std::abs(std::abs(v.position[2]) - 3.0f) < 1e-5f);
  }

  // A capsule is convex around its axis segment, so every normal points away
  // from the nearest point on that segment. This is what catches a lathe ring
  // whose profile normal got flipped.
  asset::Mesh capsule = asset::MakeCapsule(0.4f, 0.6f, 16, 24, id);
  for (const asset::Vertex& v : capsule.lods[0].vertices) {
    f32 axis_y = v.position[1] > 0.6f ? 0.6f : (v.position[1] < -0.6f ? -0.6f : v.position[1]);
    f32 d[3] = {v.position[0], v.position[1] - axis_y, v.position[2]};
    f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    CHECK(std::abs(len - 0.4f) < 1e-3f);  // every point is one radius off the axis
    CHECK((d[0] * v.normal[0] + d[1] * v.normal[1] + d[2] * v.normal[2]) / len > 0.999f);
  }

  // A torus normal points away from the ring circle it wraps, which the convex
  // test above cannot express (the inner half faces the axis).
  asset::Mesh torus = asset::MakeTorus(1.0f, 0.25f, 20, 32, id);
  for (const asset::Vertex& v : torus.lods[0].vertices) {
    f32 radial = std::sqrt(v.position[0] * v.position[0] + v.position[2] * v.position[2]);
    f32 scale = radial > 1e-6f ? 1.0f / radial : 0.0f;
    f32 d[3] = {v.position[0] - v.position[0] * scale, v.position[1],
                v.position[2] - v.position[2] * scale};
    f32 len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    CHECK(std::abs(len - 0.25f) < 1e-3f);
    CHECK((d[0] * v.normal[0] + d[1] * v.normal[1] + d[2] * v.normal[2]) / len > 0.999f);
  }

  // The cone's apex ring collapses onto the axis; the triangles that would
  // collapse with it must not be emitted.
  asset::Mesh cone = asset::MakeCone(0.5f, 1.0f, 24, id);
  const asset::MeshLod& cone_lod = cone.lods[0];
  for (size_t i = 0; i + 2 < cone_lod.indices.size(); i += 3) {
    const asset::Vertex& a = cone_lod.vertices[cone_lod.indices[i]];
    const asset::Vertex& b = cone_lod.vertices[cone_lod.indices[i + 1]];
    const asset::Vertex& c = cone_lod.vertices[cone_lod.indices[i + 2]];
    f32 e1[3], e2[3];
    for (int k = 0; k < 3; ++k) {
      e1[k] = b.position[k] - a.position[k];
      e2[k] = c.position[k] - a.position[k];
    }
    f32 n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                e1[0] * e2[1] - e1[1] * e2[0]};
    CHECK(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) > 1e-6f);
  }
}

// A sphere cut into a top and a bottom material plus a patch of a third, which
// is what an authored building is: several materials on one surface, one of
// them a detail smaller than a lod cell.
asset::Mesh MakeThreeMaterialSphere() {
  asset::Mesh mesh = asset::MakeSphere(1.0f, 60, 90, asset::MakeAssetId("m"));
  asset::MeshLod& lod = mesh.lods[0];
  const u32 total = static_cast<u32>(lod.indices.size());
  const u32 seam = total / 6 * 3;  // ring boundary between the two halves
  const u32 trim = 6;              // two triangles: one quad of the sphere
  lod.submeshes.clear();
  lod.submeshes.push_back({0, seam, asset::MakeAssetId("wall")});
  lod.submeshes.push_back({seam, trim, asset::MakeAssetId("trim")});
  lod.submeshes.push_back({seam + trim, total - seam - trim, asset::MakeAssetId("roof")});
  return mesh;
}

// A decimated multi-submesh mesh has to keep its materials and their index
// ranges, and must not weld a vertex across two of them - the pair would then
// have to draw with one material.
void TestMultiSubmeshLods() {
  asset::Mesh mesh = MakeThreeMaterialSphere();
  const asset::MeshLod fine = mesh.lods[0];
  asset::GenerateLods(&mesh);
  CHECK(mesh.lods.size() > 1);

  for (size_t l = 1; l < mesh.lods.size(); ++l) {
    const asset::MeshLod& lod = mesh.lods[l];
    CHECK(lod.indices.size() % 3 == 0);
    CHECK(lod.indices.size() < mesh.lods[l - 1].indices.size());
    // Entry for entry with lod 0, in order: the draw loop pairs submesh k of a
    // coarse lod with submesh k of lod 0 and takes the material from there.
    CHECK(lod.submeshes.size() == fine.submeshes.size());
    if (lod.submeshes.size() != fine.submeshes.size()) continue;
    u32 covered = 0;
    for (size_t s = 0; s < lod.submeshes.size(); ++s) {
      CHECK(lod.submeshes[s].material == fine.submeshes[s].material);
      CHECK(lod.submeshes[s].index_offset == covered);
      covered += lod.submeshes[s].index_count;
    }
    CHECK(covered == lod.indices.size());
    for (u32 index : lod.indices) CHECK(index < lod.vertices.size());

    // No vertex is reachable from two submeshes.
    base::Vector<int> owner(lod.vertices.size());
    for (int& o : owner) o = -1;
    bool shared = false;
    for (size_t s = 0; s < lod.submeshes.size(); ++s) {
      const asset::Submesh& sub = lod.submeshes[s];
      for (u32 i = sub.index_offset; i < sub.index_offset + sub.index_count; ++i) {
        int& o = owner[lod.indices[i]];
        shared |= o >= 0 && o != static_cast<int>(s);
        o = static_cast<int>(s);
      }
    }
    CHECK(!shared);

    // The wall and the roof still meet: their representatives along the seam
    // are separate vertices but sit on the same points, or the lod cracks open
    // along every material boundary in the scene.
    bool welded = false;
    for (u32 a = 0; a < lod.vertices.size(); ++a) {
      if (owner[a] != 0) continue;
      for (u32 b = 0; b < lod.vertices.size(); ++b) {
        if (owner[b] != 2) continue;
        welded |= lod.vertices[a].position[0] == lod.vertices[b].position[0] &&
                  lod.vertices[a].position[1] == lod.vertices[b].position[1] &&
                  lod.vertices[a].position[2] == lod.vertices[b].position[2];
      }
    }
    CHECK(welded);

    // The trim is a patch narrower than a coarse cell, so it folds up entirely
    // - and still has to hold its slot rather than shifting the roof after it.
    if (l + 1 == mesh.lods.size()) CHECK(lod.submeshes[1].index_count == 0);
  }

  // Loose cards (alpha-masked foliage) do not cluster: the grid cannot merge
  // vertices that no triangle shares, so the mesh is left at one lod rather
  // than handed a coarser grid that only shreds the cards.
  asset::Mesh cards;
  cards.id = asset::MakeAssetId("cards");
  asset::MeshLod& card_lod = cards.lods.emplace_back();
  for (u32 i = 0; i < 400; ++i) {
    const f32 x = static_cast<f32>(i % 20), z = static_cast<f32>(i / 20);
    const u32 base = static_cast<u32>(card_lod.vertices.size());
    for (u32 c = 0; c < 4; ++c) {
      asset::Vertex v{};
      v.position[0] = x + ((c & 1) ? 0.9f : 0.0f);
      v.position[1] = (c & 2) ? 0.9f : 0.0f;
      v.position[2] = z;
      v.normal[2] = 1.0f;
      v.tangent[0] = 1.0f;
      v.tangent[3] = 1.0f;
      card_lod.vertices.push_back(v);
    }
    for (u32 c : {0u, 1u, 2u, 1u, 3u, 2u}) card_lod.indices.push_back(base + c);
  }
  card_lod.submeshes.push_back({0, static_cast<u32>(card_lod.indices.size()),
                                asset::MakeAssetId("leaf")});
  asset::GenerateLods(&cards);
  CHECK(cards.lods.size() == 1);
}

}  // namespace

int main() {
  TestPatternNames();
  TestCheckerMask();
  TestPatternAxes();
  TestPatternRange();
  TestColorTexture();
  TestNormalMap();
  TestRoughnessMap();
  TestPrimitives();
  TestMultiSubmeshLods();
  if (failures == 0) std::printf("procedural_asset_test: PASS\n");
  return failures == 0 ? 0 : 1;
}
