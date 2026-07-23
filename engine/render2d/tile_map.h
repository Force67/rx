#ifndef RX_RENDER2D_TILE_MAP_H_
#define RX_RENDER2D_TILE_MAP_H_

#include <cmath>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"
#include "render2d/types2d.h"

namespace rx::render2d {

// A tileset: an atlas image sliced into a fixed grid of tiles. Tile ids run
// left-to-right, top-to-bottom, 0-based; a negative id is "empty". Pure data +
// uv math, so it is unit-testable without a GPU.
struct Tileset {
  u32 tile_w = 16, tile_h = 16;    // tile size in atlas texels
  u32 columns = 1, rows = 1;       // atlas grid dimensions
  u32 atlas_w = 16, atlas_h = 16;  // atlas size in texels

  bool HasTile(i32 id) const {
    const u64 count = static_cast<u64>(columns) * rows;
    return id >= 0 && columns > 0 && rows > 0 && atlas_w > 0 && atlas_h > 0 && tile_w > 0 &&
           tile_h > 0 && columns <= atlas_w / tile_w && rows <= atlas_h / tile_h &&
           static_cast<u64>(id) < count;
  }

  // Uv sub-rect (origin + extent, 0..1) of a tile id. Empty if invalid.
  Rect TileUv(i32 id) const {
    if (!HasTile(id)) return {};
    u32 col = static_cast<u32>(id) % columns;
    u32 row = static_cast<u32>(id) / columns;
    return {static_cast<f32>(col * tile_w) / atlas_w, static_cast<f32>(row * tile_h) / atlas_h,
            static_cast<f32>(tile_w) / atlas_w, static_cast<f32>(tile_h) / atlas_h};
  }
};

// One layer of a tilemap: a width x height grid of tile ids drawn at a fixed
// painter-order key, with an optional parallax factor (< 1 for distant
// background layers) and a collision flag gameplay queries.
struct TileLayer {
  u32 width = 0, height = 0;
  f32 sort_key = 0.5f;
  f32 parallax = 1.0f;
  bool collision = false;
  base::Vector<i32> tiles;  // width*height, row-major, -1 == empty

  i32 At(i32 x, i32 y) const {
    if (x < 0 || y < 0 || x >= static_cast<i32>(width) || y >= static_cast<i32>(height)) return -1;
    return tiles[static_cast<u32>(y) * width + static_cast<u32>(x)];
  }
  void Set(i32 x, i32 y, i32 id) {
    if (x < 0 || y < 0 || x >= static_cast<i32>(width) || y >= static_cast<i32>(height)) return;
    tiles[static_cast<u32>(y) * width + static_cast<u32>(x)] = id;
  }
};

// A tilemap: a stack of layers over one shared tileset. tile_size is the world
// extent of one tile (world units per tile), independent of the tileset's texel
// size, so art resolution and world scale decouple.
class RX_RENDER2D_EXPORT TileMap {
 public:
  Tileset tileset;
  f32 tile_size = 16.0f;
  base::Vector<TileLayer> layers;

  // Appends a layer (all tiles empty) and returns it by reference. The returned
  // reference is valid only until the next AddLayer (the layer vector may
  // reallocate); to keep editing an older layer, index `layers` instead.
  TileLayer& AddLayer(u32 w, u32 h, f32 sort_key, f32 parallax = 1.0f,
                      bool collision = false);

  Vec2i WorldToTile(Vec2 world) const;
  // Top-left world corner of tile (tx, ty).
  Vec2 TileToWorld(i32 tx, i32 ty) const {
    if (!(tile_size > 0.0f) || !std::isfinite(tile_size)) return {};
    return {static_cast<f32>(tx) * tile_size, static_cast<f32>(ty) * tile_size};
  }

  // A tile is solid if any collision layer has a non-empty id there.
  bool IsSolidTile(i32 tx, i32 ty) const;
  bool IsSolidWorld(Vec2 world) const;

  // World bounds covered by the widest layer, for camera clamping.
  Rect WorldBounds() const;
};

}  // namespace rx::render2d

#endif  // RX_RENDER2D_TILE_MAP_H_
