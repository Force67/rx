#include "render2d/tile_map.h"

#include <cmath>

namespace rx::render2d {

TileLayer& TileMap::AddLayer(u32 w, u32 h, f32 depth, f32 parallax, bool collision) {
  TileLayer layer;
  layer.width = w;
  layer.height = h;
  layer.depth = depth;
  layer.parallax = parallax;
  layer.collision = collision;
  layer.tiles.resize(static_cast<size_t>(w) * h, -1);
  layers.push_back(std::move(layer));
  return layers.back();
}

Vec2i TileMap::WorldToTile(Vec2 world) const {
  return {static_cast<i32>(std::floor(world.x / tile_size)),
          static_cast<i32>(std::floor(world.y / tile_size))};
}

bool TileMap::IsSolidTile(i32 tx, i32 ty) const {
  for (const TileLayer& layer : layers) {
    if (!layer.collision) continue;
    if (layer.At(tx, ty) >= 0) return true;
  }
  return false;
}

Rect TileMap::WorldBounds() const {
  u32 max_w = 0, max_h = 0;
  for (const TileLayer& layer : layers) {
    max_w = layer.width > max_w ? layer.width : max_w;
    max_h = layer.height > max_h ? layer.height : max_h;
  }
  return {0, 0, static_cast<f32>(max_w) * tile_size, static_cast<f32>(max_h) * tile_size};
}

}  // namespace rx::render2d
