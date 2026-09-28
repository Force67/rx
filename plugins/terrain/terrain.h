#ifndef RX_TERRAIN_TERRAIN_H_
#define RX_TERRAIN_TERRAIN_H_

#include <math.h>

#include <base/containers/vector.h>

#include "base/containers/array.h"
#include "base/containers/span.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/math/math.h"
#include "rxe/asset/asset_id.h"
#include "rxe/asset/mesh.h"
#include "rxe/scene/world_streaming.h"

namespace rx::terrain {

struct TerrainTileKey {
  i32 x = 0;
  i32 z = 0;

  bool operator==(const TerrainTileKey &) const = default;
};

struct TerrainWeights {
  base::Array<u8, 4> rgba = {255, 0, 0, 0};

  bool operator==(const TerrainWeights &) const = default;
};

struct TerrainLayer {
  base::String name;
  asset::AssetId albedo;
  asset::AssetId normal;
  base::Array<u8, 4> debug_rgba = {96, 128, 64, 255};

  bool operator==(const TerrainLayer &) const = default;
};

struct TerrainDesc {
  asset::AssetId id;
  Vec3 origin;
  u32 tile_quads = 32;
  f32 sample_spacing = 1.0f;
  base::Vector<TerrainLayer> layers;
};

struct TerrainTile {
  TerrainTileKey key;
  base::Vector<f32> heights;
  base::Vector<TerrainWeights> weights;
  f32 minimum_height = 0;
  f32 maximum_height = 0;
  u64 revision = 0;
};

enum class TerrainBrushMode : u8 {
  kRaise,
  kLower,
  kSmooth,
  kFlatten,
  kPaintLayer,
};

struct TerrainBrush {
  TerrainBrushMode mode = TerrainBrushMode::kRaise;
  f32 center_x = 0;
  f32 center_z = 0;
  f32 radius = 1;
  f32 strength = 1;
  // Exponent applied to 1-distance/radius. Zero gives a hard brush.
  f32 falloff = 1;
  // World-space y used only by kFlatten.
  f32 flatten_target = 0;
  u32 layer = 0;
};

struct TerrainSampleState {
  f32 height = 0;
  TerrainWeights weights;

  bool operator==(const TerrainSampleState &) const = default;
};

struct TerrainSampleChange {
  TerrainTileKey tile;
  u32 sample = 0;
  TerrainSampleState old_value;
  TerrainSampleState new_value;
};

struct TerrainChange {
  asset::AssetId terrain;
  base::Vector<TerrainSampleChange> samples;
  base::Vector<TerrainTileKey> dirty_tiles;

  bool empty() const { return samples.empty(); }
};

struct TerrainRayHit {
  Vec3 position;
  Vec3 normal;
  TerrainTileKey tile;
  f32 distance = 0;
};

class RX_TERRAIN_EXPORT Terrain {
public:
  Terrain();
  explicit Terrain(TerrainDesc desc);

  const TerrainDesc &desc() const { return desc_; }
  base::Span<const TerrainTile> tiles() const {
    return base::Span(tiles_.data(), tiles_.size());
  }
  u32 samples_per_side() const { return desc_.tile_quads + 1; }

  const TerrainTile *FindTile(TerrainTileKey key) const;

  // Replaces a tile and makes its supplied border authoritative over any
  // existing cardinal or diagonal neighbors. Empty weights select layer zero.
  bool AddOrReplaceTile(TerrainTileKey key, base::Span<const f32> heights,
                        base::Span<const TerrainWeights> weights = {});

  base::Optional<f32> SampleHeight(f32 world_x, f32 world_z) const;
  asset::AssetId TileAssetId(TerrainTileKey key) const;
  base::Optional<scene::WorldStreamRegion> TileRegion(TerrainTileKey key,
                                                     u32 channels = ~u32{0},
                                                     i32 priority = 0) const;

  // Replaces `regions` with the sorted conservative broad-phase candidates for
  // the query's swept sphere.
  void GatherStreamRegions(const scene::WorldStreamQuery &query,
                           base::Vector<scene::WorldStreamRegion> *regions,
                           u32 channels = ~u32{0}, i32 priority = 0) const;

  base::Optional<asset::Mesh> BuildTileMesh(TerrainTileKey key,
                                           asset::AssetId material) const;
  base::Optional<TerrainRayHit>
  Raycast(Vec3 origin, Vec3 direction,
          f32 maximum_distance = INFINITY) const;

  TerrainChange ApplyBrush(const TerrainBrush &brush);
  bool ApplyChange(const TerrainChange &change);
  bool RevertChange(const TerrainChange &change);

private:
  TerrainTile *FindTileMutable(TerrainTileKey key);
  base::Optional<f32> GridHeight(i64 grid_x, i64 grid_z) const;
  void RecalculateBounds(TerrainTile *tile);
  bool SetChangeState(const TerrainChange &change, bool use_new);

  TerrainDesc desc_;
  base::Vector<TerrainTile> tiles_;

  friend bool LoadTerrain(const base::String &, Terrain *, base::String *);
};

// Appends a later, already-applied dab to a stroke. Repeated samples retain the
// stroke's first old value and the dab's final new value.
RX_TERRAIN_EXPORT bool MergeTerrainChanges(TerrainChange *stroke,
                                           const TerrainChange &dab);

RX_TERRAIN_EXPORT bool SaveTerrain(const Terrain &terrain,
                                   const base::String &file_path,
                                   base::String *error = nullptr);
RX_TERRAIN_EXPORT bool LoadTerrain(const base::String &file_path,
                                   Terrain *terrain,
                                   base::String *error = nullptr);

} // namespace rx::terrain

#endif // RX_TERRAIN_TERRAIN_H_
