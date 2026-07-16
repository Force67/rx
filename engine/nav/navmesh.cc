#include "nav/navmesh.h"

#include <algorithm>
#include <cmath>

namespace rx::nav {

NavMesh::NavMesh(const NavMeshConfig& config) : config_(config) {
  for (u32 i = 0; i < kMaxAreas; ++i) {
    traverse_mult_[i] = 1.0f;
    entry_cost_[i] = 0.0f;
  }
}

void NavMesh::SetAreaCost(AreaId area, f32 traverse_multiplier, f32 entry_cost) {
  if (area >= kMaxAreas) return;
  traverse_mult_[area] = std::max(traverse_multiplier, 0.01f);
  entry_cost_[area] = std::max(entry_cost, 0.0f);
}

bool NavMesh::BuildTile(i32 tx, i32 tz, SampleFn& sample) {
  const u32 n = config_.tile_cells;
  const f32 cs = config_.cell_size;
  Tile tile;
  tile.version = ++version_counter_;
  tile.height.resize(n * n, 0.0f);
  tile.area.resize(n * n, kAreaNone);
  bool any = false;
  for (u32 cz = 0; cz < n; ++cz) {
    for (u32 cx = 0; cx < n; ++cx) {
      const f32 wx = (static_cast<f32>(tx) * n + cx + 0.5f) * cs;
      const f32 wz = (static_cast<f32>(tz) * n + cz + 0.5f) * cs;
      Sample s;
      if (!sample(wx, wz, s) || s.area == kAreaNone) continue;
      tile.height[cz * n + cx] = s.height;
      tile.area[cz * n + cx] = static_cast<AreaId>(s.area & (kMaxAreas - 1));
      any = true;
    }
  }
  tiles_[PackTile(tx, tz)] = static_cast<Tile&&>(tile);
  return any;
}

u32 NavMesh::EnsureBubble(const Vec3& center, f32 radius, SampleFn& sample, u32 max_tiles) {
  const f32 tile_m = config_.cell_size * static_cast<f32>(config_.tile_cells);
  const i32 t0x = static_cast<i32>(std::floor((center.x - radius) / tile_m));
  const i32 t1x = static_cast<i32>(std::floor((center.x + radius) / tile_m));
  const i32 t0z = static_cast<i32>(std::floor((center.z - radius) / tile_m));
  const i32 t1z = static_cast<i32>(std::floor((center.z + radius) / tile_m));

  // Missing tiles, then nearest-first so the ground under the agents exists
  // before the bubble rim does.
  struct Want {
    i32 tx, tz;
    f32 dist2;
  };
  base::Vector<Want> missing;
  for (i32 tz = t0z; tz <= t1z; ++tz) {
    for (i32 tx = t0x; tx <= t1x; ++tx) {
      if (tiles_.find(PackTile(tx, tz))) continue;
      const f32 cx = (static_cast<f32>(tx) + 0.5f) * tile_m - center.x;
      const f32 cz = (static_cast<f32>(tz) + 0.5f) * tile_m - center.z;
      const f32 d2 = cx * cx + cz * cz;
      const f32 reach = radius + tile_m * 0.7072f;  // tile half-diagonal
      if (d2 > reach * reach) continue;
      missing.push_back({tx, tz, d2});
    }
  }
  std::sort(missing.begin(), missing.end(),
            [](const Want& a, const Want& b) { return a.dist2 < b.dist2; });
  u32 built = 0;
  for (const Want& w : missing) {
    if (built >= max_tiles) break;
    BuildTile(w.tx, w.tz, sample);
    ++built;
  }
  return built;
}

u32 NavMesh::RemoveTilesBeyond(const Vec3& center, f32 radius) {
  const f32 tile_m = config_.cell_size * static_cast<f32>(config_.tile_cells);
  base::Vector<u64> drop;
  tiles_.ForEach([&](const u64& key, const Tile&) {
    const f32 cx = (static_cast<f32>(UnpackX(key)) + 0.5f) * tile_m - center.x;
    const f32 cz = (static_cast<f32>(UnpackZ(key)) + 0.5f) * tile_m - center.z;
    const f32 reach = radius + tile_m * 0.7072f;
    if (cx * cx + cz * cz > reach * reach) drop.push_back(key);
  });
  for (u64 key : drop) tiles_.erase(key);
  return static_cast<u32>(drop.size());
}

void NavMesh::PaintDisc(const Vec3& center, f32 radius, AreaId area) {
  if (area >= kMaxAreas) return;
  const f32 cs = config_.cell_size;
  const i32 c0x = static_cast<i32>(std::floor((center.x - radius) / cs));
  const i32 c1x = static_cast<i32>(std::floor((center.x + radius) / cs));
  const i32 c0z = static_cast<i32>(std::floor((center.z - radius) / cs));
  const i32 c1z = static_cast<i32>(std::floor((center.z + radius) / cs));
  const i32 n = static_cast<i32>(config_.tile_cells);
  u64 last_tile = ~0ull;
  Tile* tile = nullptr;
  for (i32 cz = c0z; cz <= c1z; ++cz) {
    for (i32 cx = c0x; cx <= c1x; ++cx) {
      const f32 wx = (static_cast<f32>(cx) + 0.5f) * cs - center.x;
      const f32 wz = (static_cast<f32>(cz) + 0.5f) * cs - center.z;
      if (wx * wx + wz * wz > radius * radius) continue;
      const u64 key = PackTile(FloorDiv(cx, n), FloorDiv(cz, n));
      if (key != last_tile) {
        tile = tiles_.find(key);
        last_tile = key;
        if (tile) tile->version = ++version_counter_;
      }
      if (!tile) continue;
      const u32 lx = static_cast<u32>(cx - FloorDiv(cx, n) * n);
      const u32 lz = static_cast<u32>(cz - FloorDiv(cz, n) * n);
      AreaId& cell = tile->area[lz * static_cast<u32>(n) + lx];
      if (cell != kAreaNone) cell = area;  // paint changes desirability, not holes
    }
  }
}

const NavMesh::Tile* NavMesh::TileOf(CellRef cell, u32* out_index) const {
  if (!cell.valid()) return nullptr;
  const i32 n = static_cast<i32>(config_.tile_cells);
  const Tile* tile = tiles_.find(PackTile(FloorDiv(cell.x, n), FloorDiv(cell.z, n)));
  if (!tile) return nullptr;
  const u32 lx = static_cast<u32>(cell.x - FloorDiv(cell.x, n) * n);
  const u32 lz = static_cast<u32>(cell.z - FloorDiv(cell.z, n) * n);
  *out_index = lz * static_cast<u32>(n) + lx;
  return tile;
}

u32 NavMesh::TileVersion(CellRef cell) const {
  u32 index = 0;
  const Tile* tile = TileOf(cell, &index);
  return tile ? tile->version : 0;
}

u32 NavMesh::TileVersionAt(i32 tx, i32 tz) const {
  const Tile* tile = tiles_.find(PackTile(tx, tz));
  return tile ? tile->version : 0;
}

u64 NavMesh::TileKeyOf(CellRef cell) const {
  const i32 n = static_cast<i32>(config_.tile_cells);
  return PackTile(FloorDiv(cell.x, n), FloorDiv(cell.z, n));
}

u32 NavMesh::TileVersionByKey(u64 key) const {
  const Tile* tile = tiles_.find(key);
  return tile ? tile->version : 0;
}

CellRef NavMesh::CellAt(const Vec3& pos) const {
  const f32 cs = config_.cell_size;
  return {static_cast<i32>(std::floor(pos.x / cs)), static_cast<i32>(std::floor(pos.z / cs))};
}

Vec3 NavMesh::CellCenter(CellRef cell) const {
  const f32 cs = config_.cell_size;
  f32 y = CellHeight(cell);
  if (std::isnan(y)) y = 0;
  return {(static_cast<f32>(cell.x) + 0.5f) * cs, y, (static_cast<f32>(cell.z) + 0.5f) * cs};
}

AreaId NavMesh::Area(CellRef cell) const {
  u32 index = 0;
  const Tile* tile = TileOf(cell, &index);
  return tile ? tile->area[index] : kAreaNone;
}

f32 NavMesh::CellHeight(CellRef cell) const {
  u32 index = 0;
  const Tile* tile = TileOf(cell, &index);
  if (!tile || tile->area[index] == kAreaNone) return NAN;
  return tile->height[index];
}

bool NavMesh::HeightAt(f32 x, f32 z, f32* out_height) const {
  const f32 cs = config_.cell_size;
  // Sample space where integer coordinates sit on cell centers.
  const f32 sx = x / cs - 0.5f;
  const f32 sz = z / cs - 0.5f;
  const i32 x0 = static_cast<i32>(std::floor(sx));
  const i32 z0 = static_cast<i32>(std::floor(sz));
  const f32 fx = sx - static_cast<f32>(x0);
  const f32 fz = sz - static_cast<f32>(z0);
  f32 h[4];
  f32 w[4] = {(1 - fx) * (1 - fz), fx * (1 - fz), (1 - fx) * fz, fx * fz};
  const CellRef cells[4] = {{x0, z0}, {x0 + 1, z0}, {x0, z0 + 1}, {x0 + 1, z0 + 1}};
  f32 weight_sum = 0, height_sum = 0;
  for (int i = 0; i < 4; ++i) {
    h[i] = CellHeight(cells[i]);
    if (std::isnan(h[i])) continue;
    weight_sum += w[i];
    height_sum += h[i] * w[i];
  }
  if (weight_sum <= 0) return false;
  *out_height = height_sum / weight_sum;
  return true;
}

bool NavMesh::Reachable(CellRef from, CellRef to) const {
  u32 ia = 0, ib = 0;
  const Tile* ta = TileOf(from, &ia);
  const Tile* tb = TileOf(to, &ib);
  if (!ta || !tb || ta->area[ia] == kAreaNone || tb->area[ib] == kAreaNone) return false;
  return std::fabs(ta->height[ia] - tb->height[ib]) <= config_.max_step;
}

CellRef NavMesh::ClampToWalkable(const Vec3& pos, f32 max_radius) const {
  const CellRef at = CellAt(pos);
  if (Walkable(at)) return at;
  const i32 rings = static_cast<i32>(std::ceil(max_radius / config_.cell_size));
  // Ring scan outward; within a ring prefer the cell nearest to pos.
  for (i32 r = 1; r <= rings; ++r) {
    CellRef best;
    f32 best_d2 = 0;
    auto consider = [&](i32 cx, i32 cz) {
      const CellRef c{cx, cz};
      if (!Walkable(c)) return;
      const Vec3 center = CellCenter(c);
      const f32 dx = center.x - pos.x;
      const f32 dz = center.z - pos.z;
      const f32 d2 = dx * dx + dz * dz;
      if (!best.valid() || d2 < best_d2) {
        best = c;
        best_d2 = d2;
      }
    };
    for (i32 i = -r; i <= r; ++i) {
      consider(at.x + i, at.z - r);
      consider(at.x + i, at.z + r);
      if (i != -r && i != r) {
        consider(at.x - r, at.z + i);
        consider(at.x + r, at.z + i);
      }
    }
    if (best.valid()) return best;
  }
  return {};
}

f32 NavMesh::StepCost(CellRef from, CellRef to) const {
  const f32 cs = config_.cell_size;
  const bool diagonal = (from.x != to.x) && (from.z != to.z);
  const f32 dist = diagonal ? cs * 1.41421356f : cs;
  const AreaId a = Area(from);
  const AreaId b = Area(to);
  f32 cost = dist * 0.5f * (TraverseMultiplier(a) + TraverseMultiplier(b));
  if (a != b) cost += EntryCost(b);  // one-time toll for crossing INTO b
  return cost;
}

NavRaycast NavMesh::Raycast(const Vec3& from, const Vec3& to) const {
  NavRaycast result;
  const f32 cs = config_.cell_size;
  CellRef cell = CellAt(from);
  const CellRef end = CellAt(to);
  if (!Walkable(cell)) return result;

  // Amanatides & Woo traversal over the cell grid.
  const f32 dx = to.x - from.x;
  const f32 dz = to.z - from.z;
  const f32 len = std::sqrt(dx * dx + dz * dz);
  const i32 step_x = dx > 0 ? 1 : -1;
  const i32 step_z = dz > 0 ? 1 : -1;
  const f32 inv_dx = dx != 0 ? 1.0f / dx : 0;
  const f32 inv_dz = dz != 0 ? 1.0f / dz : 0;
  f32 t_max_x = dx != 0 ? ((static_cast<f32>(cell.x + (step_x > 0)) * cs) - from.x) * inv_dx : 2.0f;
  f32 t_max_z = dz != 0 ? ((static_cast<f32>(cell.z + (step_z > 0)) * cs) - from.z) * inv_dz : 2.0f;
  const f32 t_delta_x = dx != 0 ? cs * static_cast<f32>(step_x) * inv_dx : 0;
  const f32 t_delta_z = dz != 0 ? cs * static_cast<f32>(step_z) * inv_dz : 0;

  f32 t_prev = 0;
  AreaId prev_area = Area(cell);
  // Generous bound: the traversal visits at most dx+dz+1 cells.
  const i32 max_steps =
      std::abs(end.x - cell.x) + std::abs(end.z - cell.z) + 2;
  for (i32 i = 0; i < max_steps && !(cell == end); ++i) {
    CellRef next = cell;
    f32 t;
    if (t_max_x < t_max_z) {
      t = std::min(t_max_x, 1.0f);
      next.x += step_x;
      t_max_x += t_delta_x;
    } else {
      t = std::min(t_max_z, 1.0f);
      next.z += step_z;
      t_max_z += t_delta_z;
    }
    if (!Reachable(cell, next)) return result;  // clear stays false
    const f32 seg = (t - t_prev) * len;
    const AreaId area = Area(cell);
    result.cost += seg * TraverseMultiplier(area);
    if (area != prev_area) result.cost += EntryCost(area);
    result.length += seg;
    t_prev = t;
    prev_area = area;
    cell = next;
  }
  if (!(cell == end)) return result;  // bound hit without arriving
  const AreaId area = Area(cell);
  result.cost += (1.0f - t_prev) * len * TraverseMultiplier(area);
  if (area != prev_area) result.cost += EntryCost(area);
  result.length += (1.0f - t_prev) * len;
  result.clear = true;
  return result;
}

}  // namespace rx::nav
