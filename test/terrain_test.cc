#include "terrain/terrain.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace rx::terrain;
namespace asset = rx::asset;
namespace scene = rx::scene;
using rx::f32;
using rx::u32;
using rx::u64;
using rx::u8;
using rx::Vec3;

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "terrain_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char *message, f32 epsilon = 1e-4f) {
  if (std::abs(actual - expected) <= epsilon)
    return;
  std::fprintf(stderr, "terrain_test: FAIL: %s (got %.6f, expected %.6f)\n",
               message, actual, expected);
  ++failures;
}

TerrainDesc BasicDesc() {
  TerrainDesc desc;
  desc.id = asset::AssetId{0x123456789abcdef0ull};
  desc.tile_quads = 2;
  desc.sample_spacing = 1;
  desc.layers.push_back(TerrainLayer{"Ground", {}, {}, {255, 0, 0, 255}});
  desc.layers.push_back(TerrainLayer{"Rock", {}, {}, {0, 0, 255, 255}});
  return desc;
}

u32 WeightSum(TerrainWeights weights) {
  return weights.rgba[0] + weights.rgba[1] + weights.rgba[2] + weights.rgba[3];
}

void TestSeamsAndBrushes() {
  Terrain terrain(BasicDesc());
  const std::array<f32, 9> flat = {};
  const std::array<f32, 9> right = {3, 3, 3, 4, 4, 4, 5, 5, 5};
  Check(terrain.AddOrReplaceTile({0, 0}, flat), "left tile is added");
  Check(terrain.AddOrReplaceTile({1, 0}, right), "right tile is added");
  const TerrainTile *left = terrain.FindTile({0, 0});
  const TerrainTile *neighbor = terrain.FindTile({1, 0});
  Check(left && neighbor, "both neighboring tiles can be found");
  if (!left || !neighbor)
    return;
  for (u32 z = 0; z < 3; ++z) {
    Near(left->heights[z * 3 + 2], neighbor->heights[z * 3],
         "adding a neighbor synchronizes its shared edge");
  }

  const u64 left_revision = left->revision;
  const u64 right_revision = neighbor->revision;
  TerrainBrush raise;
  raise.mode = TerrainBrushMode::kRaise;
  raise.center_x = 2;
  raise.center_z = 1;
  raise.radius = 0.2f;
  raise.strength = 2;
  raise.falloff = 0;
  const TerrainChange raise_change = terrain.ApplyBrush(raise);
  Check(raise_change.samples.size() == 2 &&
            raise_change.dirty_tiles.size() == 2,
        "a border dab snapshots both physical edge samples");
  left = terrain.FindTile({0, 0});
  neighbor = terrain.FindTile({1, 0});
  Near(left->heights[5], 6, "raise changes the left edge sample");
  Near(neighbor->heights[3], 6, "raise changes the matching right edge sample");
  Check(left->revision == left_revision + 1 &&
            neighbor->revision == right_revision + 1,
        "a brush increments every dirty tile revision");

  Check(terrain.RevertChange(raise_change), "raise snapshot reverts");
  Near(terrain.FindTile({0, 0})->heights[5], 4,
       "revert restores the old height");
  Check(terrain.ApplyChange(raise_change), "raise snapshot reapplies");
  Near(terrain.FindTile({1, 0})->heights[3], 6,
       "apply restores the new height");

  TerrainBrush first_brush = raise;
  first_brush.strength = 1;
  const TerrainChange first = terrain.ApplyBrush(first_brush);
  TerrainBrush second_brush = raise;
  second_brush.mode = TerrainBrushMode::kLower;
  second_brush.strength = 0.25f;
  const TerrainChange second = terrain.ApplyBrush(second_brush);
  TerrainChange stroke = first;
  Check(MergeTerrainChanges(&stroke, second),
        "two sequential dabs merge into one stroke");
  Check(stroke.samples.size() == 2, "merged stroke coalesces repeated samples");
  if (!stroke.samples.empty()) {
    Near(stroke.samples[0].old_value.height, 6,
         "merged stroke preserves the first old value");
    Near(stroke.samples[0].new_value.height, 6.75f,
         "merged stroke preserves the final new value");
  }
  Check(terrain.RevertChange(stroke), "merged stroke reverts both dabs");
  Near(terrain.FindTile({0, 0})->heights[5], 6,
       "merged revert returns to the pre-stroke value");
  Check(terrain.ApplyChange(stroke), "merged stroke reapplies");

  TerrainBrush paint = raise;
  paint.mode = TerrainBrushMode::kPaintLayer;
  paint.strength = 0.5f;
  paint.layer = 1;
  const TerrainChange paint_change = terrain.ApplyBrush(paint);
  Check(!paint_change.empty(), "paint layer creates a change");
  left = terrain.FindTile({0, 0});
  neighbor = terrain.FindTile({1, 0});
  Check(WeightSum(left->weights[5]) == 255 &&
            WeightSum(neighbor->weights[3]) == 255,
        "paint remains normalized on both sides of a seam");
  Check(left->weights[5] == neighbor->weights[3] &&
            left->weights[5].rgba[1] > 0,
        "painted border weights remain seam-free");
  Check(terrain.RevertChange(paint_change), "paint snapshot reverts");

  TerrainBrush flatten = raise;
  flatten.mode = TerrainBrushMode::kFlatten;
  flatten.center_x = 1;
  flatten.flatten_target = 9;
  flatten.strength = 1;
  const TerrainChange flatten_change = terrain.ApplyBrush(flatten);
  Near(terrain.FindTile({0, 0})->heights[4], 9,
       "flatten uses its explicit world-height target");
  Check(terrain.RevertChange(flatten_change), "flatten snapshot reverts");

  TerrainBrush smooth = flatten;
  smooth.mode = TerrainBrushMode::kSmooth;
  const TerrainChange smooth_change = terrain.ApplyBrush(smooth);
  Check(!smooth_change.empty(),
        "smooth changes a nonuniform sample neighborhood");
  Check(terrain.RevertChange(smooth_change), "smooth snapshot reverts");

  Terrain extreme(BasicDesc());
  Check(extreme.AddOrReplaceTile({std::numeric_limits<rx::i32>::max(), 0}, flat),
        "extreme positive tile key is accepted");
  TerrainBrush edge_brush = raise;
  edge_brush.center_x = static_cast<f32>(
      static_cast<double>(std::numeric_limits<rx::i32>::max()) * 2.0 + 1.0);
  edge_brush.center_z = 1;
  edge_brush.radius = 4;
  Check(!extreme.ApplyBrush(edge_brush).empty(),
        "brushing an extreme tile key avoids neighbor-key overflow");
}

void TestNeighborInvalidation() {
  Terrain terrain(BasicDesc());
  const std::array<f32, 9> left = {};
  const std::array<f32, 9> right = {0, 1, 2, 0, 1, 2, 0, 1, 2};
  Check(terrain.AddOrReplaceTile({0, 0}, left),
        "normal-invalidation left tile is added");
  Check(terrain.AddOrReplaceTile({1, 0}, right),
        "normal-invalidation right tile is added");
  const u64 revision = terrain.FindTile({0, 0})->revision;
  const std::array<f32, 9> changed_interior =
      {0, 8, 9, 0, 8, 9, 0, 8, 9};
  Check(terrain.AddOrReplaceTile({1, 0}, changed_interior),
        "normal-invalidation neighbor is replaced");
  Check(terrain.FindTile({0, 0})->revision == revision + 1,
        "same shared edge still invalidates the adjacent derived normals");
}

void TestDiagonalAndNegativeInvalidation() {
  // Negative-key cardinal border: adding {0,0} after {-1,0} synchronizes the
  // shared column through the FloorDiv/PositiveMod signed-coordinate paths.
  Terrain terrain(BasicDesc());
  const std::array<f32, 9> west = {1, 2, 3, 1, 2, 3, 1, 2, 3};
  const std::array<f32, 9> flat = {};
  Check(terrain.AddOrReplaceTile({-1, 0}, west), "negative west tile is added");
  Check(terrain.AddOrReplaceTile({0, 0}, flat), "origin tile is added");
  const TerrainTile *west_tile = terrain.FindTile({-1, 0});
  const TerrainTile *origin_tile = terrain.FindTile({0, 0});
  Check(west_tile && origin_tile, "negative-key tiles can be found");
  if (!west_tile || !origin_tile)
    return;
  for (u32 z = 0; z < 3; ++z) {
    Near(west_tile->heights[z * 3 + 2], origin_tile->heights[z * 3],
         "adding across a negative boundary synchronizes the shared column");
  }

  // Diagonal revision: replacing {0,0} with the shared corner value unchanged
  // but an edge-adjacent sample changed must still bump {-1,-1} -- its corner
  // normal reads the new tile's samples (1,0)/(0,1) through GridHeight.
  Check(terrain.AddOrReplaceTile({-1, -1}, flat), "diagonal tile is added");
  const u64 diagonal_revision = terrain.FindTile({-1, -1})->revision;
  const std::array<f32, 9> edge_changed = {0, 5, 0, 5, 0, 0, 0, 0, 0};
  Check(terrain.AddOrReplaceTile({0, 0}, edge_changed),
        "origin tile is replaced with the corner sample unchanged");
  Check(terrain.FindTile({-1, -1})->revision > diagonal_revision,
        "an unchanged shared corner still invalidates the diagonal's normals");

  // Sparse layout: {0,0} and {-1,-1} exist, {0,-1}/{-1,0} do not. A brush on
  // {0,0}'s sample (1,0) changes the diagonal's corner normal, so the change
  // must list {-1,-1} as dirty even though none of its samples moved -- the
  // usual indirection through the cardinal neighbor's border copy is absent.
  Terrain sparse(BasicDesc());
  Check(sparse.AddOrReplaceTile({0, 0}, flat), "sparse origin tile is added");
  Check(sparse.AddOrReplaceTile({-1, -1}, flat),
        "sparse diagonal tile is added");
  TerrainBrush poke;
  poke.mode = TerrainBrushMode::kRaise;
  poke.center_x = 1;
  poke.center_z = 0;
  poke.radius = 0.2f;
  poke.strength = 2;
  poke.falloff = 0;
  const TerrainChange change = sparse.ApplyBrush(poke);
  Check(!change.empty(), "sparse corner brush applies");
  bool diagonal_dirty = false;
  for (const TerrainTileKey &key : change.dirty_tiles)
    diagonal_dirty |= key.x == -1 && key.z == -1;
  Check(diagonal_dirty,
        "sparse corner brush marks the diagonal normal-dependent dirty");
}

void TestMeshAndRaycast() {
  TerrainDesc desc = BasicDesc();
  Terrain terrain(desc);
  const std::array<f32, 9> left = {0, 1, 4, 0, 1, 4, 0, 1, 4};
  const std::array<f32, 9> right = {4, 9, 16, 4, 9, 16, 4, 9, 16};
  std::array<TerrainWeights, 9> blend;
  blend.fill(TerrainWeights{{128, 127, 0, 0}});
  Check(terrain.AddOrReplaceTile({0, 0}, left, blend),
        "mesh source tile is added");
  Check(terrain.AddOrReplaceTile({1, 0}, right),
        "normal neighbor tile is added");

  const asset::AssetId material{77};
  const std::optional<asset::Mesh> mesh =
      terrain.BuildTileMesh({0, 0}, material);
  Check(mesh.has_value(), "tile mesh builds");
  if (mesh) {
    Check(mesh->lods.size() == 1 && mesh->lods[0].vertices.size() == 9 &&
              mesh->lods[0].indices.size() == 24,
          "tile mesh has grid vertex and index dimensions");
    Check(mesh->lods[0].submeshes.size() == 1 &&
              mesh->lods[0].submeshes[0].material == material,
          "tile mesh carries the supplied material submesh");
    Check(!mesh->exclude_from_rt && mesh->dynamic_vertices,
          "terrain mesh is dynamic and participates in ray tracing");
    const asset::Vertex &edge = mesh->lods[0].vertices[5];
    const Vec3 expected = Normalize(Vec3{-4, 1, 0});
    Near(edge.normal[0], expected.x,
         "edge normal uses the neighboring tile sample");
    Near(edge.normal[1], expected.y,
         "edge normal has the expected up component");
    Near(edge.uv[0], 1, "tile mesh u spans zero to one");
    Near(edge.uv[1], 0.5f, "tile mesh v spans zero to one");
    Near(edge.normal[0] * edge.tangent[0] + edge.normal[1] * edge.tangent[1] +
             edge.normal[2] * edge.tangent[2],
          0, "terrain tangent is orthogonal to its normal");
    Near(edge.tangent[3], -1,
         "terrain tangent handedness matches u=+x and v=+z");
    Check(mesh->lods[0].vertices[0].color ==
              (128u | (127u << 16) | (255u << 24)),
          "layer debug colors are blended into vertex colors");
    Near(mesh->bounds_center[0], 1, "mesh bounds center x is tile-local");
    Near(mesh->bounds_center[1], 2, "mesh bounds center includes height range");
    Near(mesh->bounds_radius, std::sqrt(6.0f),
         "mesh sphere contains xz and height extents");
  }

  Terrain ray_terrain(BasicDesc());
  const std::array<f32, 9> plane = {0, 1, 2, 2, 3, 4, 4, 5, 6};
  Check(ray_terrain.AddOrReplaceTile({0, 0}, plane), "raycast plane is added");
  const std::optional<f32> sampled = ray_terrain.SampleHeight(0.25f, 0.25f);
  Check(sampled.has_value(), "world height samples inside a sparse tile");
  if (sampled)
    Near(*sampled, 0.75f, "height sampling follows the mesh triangles");
  Check(!ray_terrain.SampleHeight(10, 10).has_value(),
        "height sampling reports sparse holes");

  const std::optional<TerrainRayHit> hit =
      ray_terrain.Raycast({0.25f, 10, 0.25f}, {0, -2, 0}, 20);
  Check(hit.has_value(), "vertical terrain ray hits");
  if (hit) {
    Near(hit->position.y, 0.75f, "raycast returns the exact triangle height");
    Near(hit->distance, 9.25f,
         "raycast normalizes direction for world distance");
    Check(hit->tile == TerrainTileKey{0, 0},
          "raycast returns the hit tile key");
    const Vec3 expected = Normalize(Vec3{-1, 1, -2});
    Near(hit->normal.x, expected.x, "raycast returns the triangle normal x");
    Near(hit->normal.y, expected.y, "raycast returns the triangle normal y");
  }
  Check(!ray_terrain.Raycast({10, 10, 10}, {0, -1, 0}, 20).has_value(),
         "tile AABB broad phase rejects a ray over a sparse hole");
  Check(!ray_terrain
             .Raycast({0.25f, 10, 0.25f},
                      {std::numeric_limits<f32>::denorm_min(), 0, 0}, 20)
             .has_value(),
        "subnormal ray directions cannot produce a non-finite hit");

  TerrainDesc idless_desc = BasicDesc();
  idless_desc.id = {};
  Terrain idless(idless_desc);
  Check(idless.AddOrReplaceTile({0, 0}, plane),
        "idless in-memory terrain accepts geometry");
  Check(idless.Raycast({0.25f, 10, 0.25f}, {0, -1, 0}, 20).has_value(),
        "geometric raycasts do not depend on a serialization identity");
}

void TestStreaming() {
  TerrainDesc desc = BasicDesc();
  desc.origin = {100, 10, -50};
  desc.sample_spacing = 2;
  Terrain terrain(desc);
  const std::array<f32, 9> near_heights = {-2, 0, 1, 0, 1, 2, 1, 2, 3};
  const std::array<f32, 9> far_heights = {};
  Check(terrain.AddOrReplaceTile({2, 0}, far_heights),
        "far stream tile is added");
  Check(terrain.AddOrReplaceTile({-1, 0}, near_heights),
        "signed stream tile is added");
  Check(terrain.tiles()[0].key == TerrainTileKey{-1, 0},
        "terrain keeps signed tile keys sorted");

  const std::optional<scene::WorldStreamRegion> region =
      terrain.TileRegion({-1, 0}, 1, 7);
  Check(region.has_value(), "terrain tile produces a stream region");
  if (region) {
    Check(region->id != 0 && region->id == terrain.TileAssetId({-1, 0}).hash,
          "stream region id is stable and nonzero");
    Near(region->minimum.x, 96, "stream bounds include signed tile x");
    Near(region->maximum.x, 100, "stream bounds include tile width");
    Near(region->minimum.y, 8, "stream bounds include minimum terrain height");
    Near(region->maximum.y, 13, "stream bounds include maximum terrain height");
    Check(region->priority == 7 && region->channels == 1,
          "stream region preserves caller metadata");
  }

  Terrain same_identity(desc);
  Check(same_identity.TileAssetId({-1, 0}) == terrain.TileAssetId({-1, 0}) &&
            terrain.TileAssetId({-1, 0}) != terrain.TileAssetId({2, 0}),
        "per-tile asset ids are stable and key-specific");

  scene::WorldStreamQuery query;
  query.origin = {95, 1000, -48};
  query.predicted = query.origin;
  query.radius = 1;
  query.channels = 1;
  query.axes = scene::kWorldStreamXZ;
  base::Vector<scene::WorldStreamRegion> candidates;
  terrain.GatherStreamRegions(query, &candidates, 1);
  Check(candidates.size() == 1 &&
            candidates[0].id == terrain.TileAssetId({-1, 0}).hash,
        "stream query filters to the intersecting sparse tile and ignores y");

  query.channels = 2;
  terrain.GatherStreamRegions(query, &candidates, 1);
  Check(candidates.empty(),
        "stream query channel mismatch returns no terrain candidates");
  query.channels = 1;
  query.predicted.x = 109;
  query.radius = 0;
  terrain.GatherStreamRegions(query, &candidates, 1);
  Check(candidates.size() == 2,
        "swept stream query conservatively gathers crossed tiles");
}

std::vector<u8> ReadBytes(const std::string &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    return {};
  const std::streamsize size = input.tellg();
  input.seekg(0);
  std::vector<u8> bytes(static_cast<size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()), size);
  return bytes;
}

void WriteBytes(const std::string &path, const std::vector<u8> &bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

u64 Checksum(const std::vector<u8> &bytes, size_t size) {
  u64 hash = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

void StoreU64(std::vector<u8> *bytes, size_t offset, u64 value) {
  for (u32 shift = 0; shift < 64; shift += 8) {
    (*bytes)[offset + shift / 8] = static_cast<u8>(value >> shift);
  }
}

void TestSerialization() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("rx-terrain-test-" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(root);
  const std::string first_path = (root / "a.rxterrain").string();
  const std::string second_path = (root / "b.rxterrain").string();
  const std::string loaded_path = (root / "loaded.rxterrain").string();
  const std::string loaded_again_path =
      (root / "loaded-again.rxterrain").string();
  const std::string corrupt_path = (root / "corrupt.rxterrain").string();
  const std::string truncated_path = (root / "truncated.rxterrain").string();
  const std::string nonfinite_path = (root / "nonfinite.rxterrain").string();
  const std::string zero_id_path = (root / "zero-id.rxterrain").string();
  const std::array<std::string, 8> paths = {
      first_path,   second_path,    loaded_path,   loaded_again_path,
      corrupt_path, truncated_path, nonfinite_path, zero_id_path};
  for (const std::string &path : paths) {
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");
  }

  TerrainDesc desc = BasicDesc();
  desc.origin = {-12.5f, 3.25f, 99};
  desc.sample_spacing = 1.5f;
  desc.layers[0].albedo = asset::AssetId{11};
  desc.layers[0].normal = asset::AssetId{12};
  desc.layers[1].albedo = asset::AssetId{21};
  desc.layers[1].normal = asset::AssetId{22};
  Terrain terrain(desc);
  const std::array<f32, 9> a = {-10.25f, -2, 0.5f, 1.25f, 3,
                                8.75f,   9,  12,   20.5f};
  const std::array<f32, 9> b = {7, 6, 5, 4, 3, 2, 1, 0, -1};
  std::array<TerrainWeights, 9> weights;
  for (u32 i = 0; i < weights.size(); ++i) {
    weights[i] =
        TerrainWeights{{static_cast<u8>(255 - i), static_cast<u8>(i), 0, 0}};
  }
  Check(terrain.AddOrReplaceTile({1, -1}, b, weights),
        "serialization tile b is added");
  Check(terrain.AddOrReplaceTile({-2, 3}, a, weights),
        "serialization tile a is added");

  std::string error;
  Check(SaveTerrain(terrain, first_path, &error), "terrain saves to rxterrain");
  Check(SaveTerrain(terrain, first_path, &error),
        "terrain atomically replaces an existing destination");
  Check(SaveTerrain(terrain, second_path, &error),
        "terrain saves a second time");
  const std::vector<u8> first_bytes = ReadBytes(first_path);
  const std::vector<u8> second_bytes = ReadBytes(second_path);
  Check(!first_bytes.empty() && first_bytes == second_bytes,
        "saving unchanged terrain produces deterministic bytes");
  Check(!std::filesystem::exists(first_path + ".tmp"),
        "successful save leaves no temporary file");

  Terrain loaded;
  Check(LoadTerrain(first_path, &loaded, &error), "saved terrain loads");
  Check(loaded.desc().id == terrain.desc().id &&
            loaded.desc().origin.x == desc.origin.x &&
            loaded.desc().origin.y == desc.origin.y &&
            loaded.desc().origin.z == desc.origin.z &&
            loaded.desc().tile_quads == desc.tile_quads &&
            loaded.desc().sample_spacing == desc.sample_spacing,
        "load recreates terrain metadata");
  Check(loaded.desc().layers == terrain.desc().layers,
        "load recreates palette names, assets, and debug colors");
  Check(loaded.tiles().size() == 2 &&
            loaded.tiles()[0].key == TerrainTileKey{-2, 3} &&
            loaded.tiles()[1].key == TerrainTileKey{1, -1},
        "load recreates sorted sparse tile keys");
  if (loaded.tiles().size() == 2) {
    const f32 quantization_error = (20.5f - (-10.25f)) / 65535.0f + 1e-5f;
    Near(loaded.tiles()[0].heights[4], a[4],
         "quantized height loads within one step", quantization_error);
    Check(loaded.tiles()[0].weights[8] == weights[8],
          "RGBA8 weights load exactly");
  }
  Check(SaveTerrain(loaded, loaded_path, &error) &&
            SaveTerrain(loaded, loaded_again_path, &error) &&
            ReadBytes(loaded_path) == ReadBytes(loaded_again_path),
        "a loaded terrain also saves deterministically");

  std::vector<u8> corrupt = first_bytes;
  if (!corrupt.empty())
    corrupt[0] ^= 0xff;
  WriteBytes(corrupt_path, corrupt);
  Terrain rejected;
  Check(!LoadTerrain(corrupt_path, &rejected, &error),
        "corrupt terrain is rejected");

  std::vector<u8> truncated = first_bytes;
  if (truncated.size() > 3)
    truncated.resize(truncated.size() - 3);
  WriteBytes(truncated_path, truncated);
  Check(!LoadTerrain(truncated_path, &rejected, &error),
        "truncated terrain is rejected");

  std::vector<u8> nonfinite = first_bytes;
  if (nonfinite.size() >= 44) {
    const u32 infinity = std::bit_cast<u32>(INFINITY);
    for (u32 shift = 0; shift < 32; shift += 8) {
      nonfinite[36 + shift / 8] = static_cast<u8>(infinity >> shift);
    }
    StoreU64(&nonfinite, nonfinite.size() - 8,
             Checksum(nonfinite, nonfinite.size() - 8));
  }
  WriteBytes(nonfinite_path, nonfinite);
  Check(!LoadTerrain(nonfinite_path, &rejected, &error),
         "non-finite terrain metadata is rejected even with a valid checksum");

  std::vector<u8> zero_id = first_bytes;
  if (zero_id.size() >= 20) {
    StoreU64(&zero_id, 12, 0);
    StoreU64(&zero_id, zero_id.size() - 8,
             Checksum(zero_id, zero_id.size() - 8));
  }
  WriteBytes(zero_id_path, zero_id);
  Check(!LoadTerrain(zero_id_path, &rejected, &error),
        "a zero terrain identity is rejected even with a valid checksum");

  for (const std::string &path : paths) {
    std::filesystem::remove(path);
    std::filesystem::remove(path + ".tmp");
  }
  std::filesystem::remove(root);
}

} // namespace

int main() {
  TestSeamsAndBrushes();
  TestNeighborInvalidation();
  TestDiagonalAndNegativeInvalidation();
  TestMeshAndRaycast();
  TestStreaming();
  TestSerialization();
  if (failures != 0) {
    std::fprintf(stderr, "terrain_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("terrain_test: PASS\n");
  return 0;
}
