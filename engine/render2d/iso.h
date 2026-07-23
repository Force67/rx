#ifndef RX_RENDER2D_ISO_H_
#define RX_RENDER2D_ISO_H_

#include <cmath>

#include "render2d/types2d.h"

namespace rx::render2d {

// Classic 2:1 dimetric ("isometric") grid<->screen mapping, in the same y-down
// world space Camera2D uses. A diamond tile of screen size (tile_w, tile_h)
// (tile_w == 2*tile_h for the canonical look) at integer grid (col, row) has
// its top-vertex/anchor at:
//
//   world.x = origin.x + (col - row) * tile_w/2
//   world.y = origin.y + (col + row) * tile_h/2
//
// Purely CPU/GPU-agnostic so it is unit-testable and shared by both the true-2D
// isometric renderer and any gameplay picking. The inverse is exact (it is a
// 2x2 linear solve), so screen picking round-trips.
struct IsoGrid {
  f32 tile_w = 64.0f;   // diamond width in world units (== pixels at zoom 1)
  f32 tile_h = 32.0f;   // diamond height
  Vec2 origin{0, 0};    // world position of grid cell (0,0)'s anchor

  // Anchor (top vertex of the diamond) of a grid cell, fractional cols allowed.
  Vec2 CellToWorld(f32 col, f32 row) const {
    return {origin.x + (col - row) * tile_w * 0.5f,
            origin.y + (col + row) * tile_h * 0.5f};
  }
  Vec2 CellToWorld(Vec2i cell) const {
    return CellToWorld(static_cast<f32>(cell.x), static_cast<f32>(cell.y));
  }

  // Diamond centre, a half-tile below the anchor - the usual sprite placement
  // and light anchor point.
  Vec2 CellCenterWorld(Vec2i cell) const {
    Vec2 a = CellToWorld(cell);
    return {a.x, a.y + tile_h * 0.5f};
  }

  // Fractional grid coordinate for a world point (invert the 2x2 map).
  Vec2 WorldToCellF(Vec2 world) const {
    f32 dx = (world.x - origin.x) / (tile_w * 0.5f);
    f32 dy = (world.y - origin.y) / (tile_h * 0.5f);
    return {(dx + dy) * 0.5f, (dy - dx) * 0.5f};
  }

  // Nearest integer cell for a world point (floor of the fractional map).
  Vec2i WorldToCell(Vec2 world) const {
    Vec2 f = WorldToCellF(world);
    return {static_cast<i32>(std::floor(f.x)), static_cast<i32>(std::floor(f.y))};
  }

  // Painter's-order sort key: cells with a larger (col+row) are nearer the
  // camera and drawn later. Objects add fractional offsets for sub-cell order.
  static f32 SortKey(f32 col, f32 row) { return col + row; }
};

}  // namespace rx::render2d

#endif  // RX_RENDER2D_ISO_H_
