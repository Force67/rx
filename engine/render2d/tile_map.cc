#include "render2d/tile_map.h"

#include <cmath>
#include <limits>

namespace rx::render2d {
namespace {

bool TryWorldToTile(f32 tile_size, Vec2 world, Vec2i* tile) {
  if (!(tile_size > 0.0f) || !std::isfinite(tile_size) ||
      !std::isfinite(world.x) || !std::isfinite(world.y)) {
    return false;
  }
  const f64 x = std::floor(static_cast<f64>(world.x) / tile_size);
  const f64 y = std::floor(static_cast<f64>(world.y) / tile_size);
  constexpr f64 kMin = std::numeric_limits<i32>::min();
  constexpr f64 kMax = std::numeric_limits<i32>::max();
  if (x < kMin || x > kMax || y < kMin || y > kMax) return false;
  *tile = {static_cast<i32>(x), static_cast<i32>(y)};
  return true;
}

}  // namespace

TileLayer& TileMap::AddLayer(u32 w, u32 h, f32 sort_key, f32 parallax, bool collision) {
  TileLayer layer;
  layer.width = w;
  layer.height = h;
  layer.sort_key = sort_key;
  layer.parallax = parallax;
  layer.collision = collision;
  layer.tiles.resize(static_cast<size_t>(w) * h, -1);
  layers.push_back(std::move(layer));
  return layers.back();
}

Vec2i TileMap::WorldToTile(Vec2 world) const {
  Vec2i tile{};
  TryWorldToTile(tile_size, world, &tile);
  return tile;
}

bool TileMap::IsSolidTile(i32 tx, i32 ty) const {
  for (const TileLayer& layer : layers) {
    if (!layer.collision) continue;
    if (layer.At(tx, ty) >= 0) return true;
  }
  return false;
}

bool TileMap::IsSolidWorld(Vec2 world) const {
  Vec2i tile;
  return TryWorldToTile(tile_size, world, &tile) &&
         IsSolidTile(tile.x, tile.y);
}

Rect TileMap::WorldBounds() const {
  if (!(tile_size > 0.0f) || !std::isfinite(tile_size)) return {};
  u32 max_w = 0, max_h = 0;
  for (const TileLayer& layer : layers) {
    max_w = layer.width > max_w ? layer.width : max_w;
    max_h = layer.height > max_h ? layer.height : max_h;
  }
  return {0, 0, static_cast<f32>(max_w) * tile_size, static_cast<f32>(max_h) * tile_size};
}

}  // namespace rx::render2d
