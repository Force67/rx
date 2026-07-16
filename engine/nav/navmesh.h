#ifndef RX_NAV_NAVMESH_H_
#define RX_NAV_NAVMESH_H_

// Tiled navigation surface for terrain-heavy worlds, after the Death
// Stranding recipe: the mesh does not only answer "can I stand here" but
// "how much do I want to". Every cell carries an area id; areas map to a
// per-meter traversal multiplier plus a one-time ENTRY cost, so pathfinding
// can prefer smooth ground, commit to rough ground once entered (no 360-turn
// backtracking after one accidental step), and wade rivers at sensible spots
// instead of treating water as a wall.
//
// Tiles are built on demand inside a bubble around the agents (EnsureBubble)
// from a game-supplied sampler -- a heightfield probe, physics raycasts,
// analytic terrain, whatever the game has. Nothing here touches physics or
// rendering; the module stays headless.
//
// Coordinates: world XZ maps to a global integer cell grid, cell (cx, cz)
// spans [cx*cell, (cx+1)*cell) in x. Tiles group tile_cells^2 cells and are
// versioned: painting or rebuilding bumps the version, which is how corridors
// notice the world changed under them (see path.h, RepathReason).

#include <cstdint>

#include <base/containers/static_function.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"

namespace rx::nav {

// Area ids partition the surface by desirability. 0 is a hole (not standable);
// 1 is plain ground. Games register more (rock, mud, shallow/deep water...)
// via NavMesh::SetAreaCost.
using AreaId = u8;
inline constexpr AreaId kAreaNone = 0;
inline constexpr AreaId kAreaGround = 1;
inline constexpr u32 kMaxAreas = 32;

// A cell on the global grid. Invalid until both coords are set (the sentinel
// stays out of any reachable range).
struct CellRef {
  i32 x = INT32_MIN;
  i32 z = INT32_MIN;
  bool valid() const { return x != INT32_MIN; }
};

inline bool operator==(const CellRef& a, const CellRef& b) { return a.x == b.x && a.z == b.z; }
inline bool operator!=(const CellRef& a, const CellRef& b) { return !(a == b); }

// What the game's sampler reports for a ground position. area == kAreaNone
// (or returning false) makes the cell a hole.
struct Sample {
  f32 height = 0;
  AreaId area = kAreaGround;
};

// Sampler contract: fill `out` for world (x, z), return false where there is
// no standable surface at all. Called once per cell during tile builds.
using SampleFn = base::StaticFunction<bool(f32 x, f32 z, Sample& out), 128>;

struct NavMeshConfig {
  f32 cell_size = 0.5f;   // meters per cell edge
  u32 tile_cells = 32;    // cells per tile edge
  f32 max_step = 0.45f;   // max height delta between adjacent walkable cells
};

// Result of a straight-line walkability test across the grid (the navmesh
// raycast used for corridor shortcutting and line-of-travel checks).
struct NavRaycast {
  bool clear = false;  // every crossed cell walkable and step-reachable
  f32 length = 0;      // planar meters actually traversed
  f32 cost = 0;        // weighted meters (multipliers + entry costs)
};

class RX_NAV_EXPORT NavMesh {
 public:
  explicit NavMesh(const NavMeshConfig& config = {});

  const NavMeshConfig& config() const { return config_; }

  // Per-meter multiplier (>= 1 keeps A* optimal) and one-time entry cost in
  // meters, charged when a path crosses INTO the area. Ids >= kMaxAreas are
  // ignored.
  void SetAreaCost(AreaId area, f32 traverse_multiplier, f32 entry_cost = 0);
  f32 TraverseMultiplier(AreaId area) const { return traverse_mult_[area & (kMaxAreas - 1)]; }
  f32 EntryCost(AreaId area) const { return entry_cost_[area & (kMaxAreas - 1)]; }

  // --- tile lifecycle -------------------------------------------------------

  // (Re)samples one tile. Returns false when the sampler reported no surface
  // anywhere in it (the tile is then stored empty, still versioned).
  bool BuildTile(i32 tx, i32 tz, SampleFn& sample);

  // Builds missing tiles intersecting the disc, nearest first, at most
  // max_tiles this call (time slicing: call every tick, the bubble fills over
  // a few frames). Returns how many were built.
  u32 EnsureBubble(const Vec3& center, f32 radius, SampleFn& sample, u32 max_tiles = 4);

  // Drops tiles fully outside the disc. Corridors through them notice via the
  // version check (a missing tile reads as version 0).
  u32 RemoveTilesBeyond(const Vec3& center, f32 radius);

  // Placement painting: stamps `area` onto every already-walkable cell in the
  // disc and bumps the touched tiles' versions. Used for procedural rocks,
  // rivers, obstacle margins.
  void PaintDisc(const Vec3& center, f32 radius, AreaId area);

  // Version of the tile owning `cell`; 0 when the tile does not exist. A
  // corridor stamps these at path time and repaths on mismatch.
  u32 TileVersion(CellRef cell) const;
  u32 TileVersionAt(i32 tx, i32 tz) const;
  // Tile key of the tile owning `cell` (the TileStamp identity) and the
  // version lookup by that key, for corridor validation.
  u64 TileKeyOf(CellRef cell) const;
  u32 TileVersionByKey(u64 key) const;

  // --- queries --------------------------------------------------------------

  CellRef CellAt(const Vec3& pos) const;
  Vec3 CellCenter(CellRef cell) const;  // y = stored cell height
  AreaId Area(CellRef cell) const;      // kAreaNone for holes / missing tiles
  bool Walkable(CellRef cell) const { return Area(cell) != kAreaNone; }
  // Cell-center height, NAN when the cell is missing.
  f32 CellHeight(CellRef cell) const;
  // Bilinear surface height across the four nearest cell centers; falls back
  // to the nearest walkable sample. Returns false when nothing is nearby.
  bool HeightAt(f32 x, f32 z, f32* out_height) const;

  // Adjacent-cell reachability: both walkable and within max_step in height.
  bool Reachable(CellRef from, CellRef to) const;

  // Nearest walkable cell to pos within max_radius meters (spiral scan).
  // The clamp target for off-mesh destinations.
  CellRef ClampToWalkable(const Vec3& pos, f32 max_radius) const;

  // Straight planar segment across the grid (supercover traversal): clear when
  // every touched cell is walkable and each hop respects max_step. Cost
  // accumulates multipliers and entry transitions like A* does.
  NavRaycast Raycast(const Vec3& from, const Vec3& to) const;

  // Weighted cost of the single step between ADJACENT cells (the A* edge
  // relaxation, exposed so corridor cost audits agree with the search).
  f32 StepCost(CellRef from, CellRef to) const;

  u32 tile_count() const { return static_cast<u32>(tiles_.size()); }

  // Visits every resident tile: fn(tx, tz, version). For debug draw.
  template <typename Fn>
  void ForEachTile(Fn&& fn) const {
    tiles_.ForEach([&](const u64& key, const Tile& tile) {
      fn(UnpackX(key), UnpackZ(key), tile.version);
    });
  }

 private:
  struct Tile {
    u32 version = 0;
    base::Vector<f32> height;   // tile_cells^2, row-major [z * tile_cells + x]
    base::Vector<AreaId> area;  // kAreaNone = hole
  };

  static u64 PackTile(i32 tx, i32 tz) {
    return (static_cast<u64>(static_cast<u32>(tx)) << 32) | static_cast<u32>(tz);
  }
  static i32 UnpackX(u64 key) { return static_cast<i32>(key >> 32); }
  static i32 UnpackZ(u64 key) { return static_cast<i32>(key & 0xffffffffu); }

  static i32 FloorDiv(i32 v, i32 d) { return (v >= 0) ? v / d : -((-v + d - 1) / d); }

  const Tile* TileOf(CellRef cell, u32* out_index) const;

  NavMeshConfig config_;
  f32 traverse_mult_[kMaxAreas];
  f32 entry_cost_[kMaxAreas];
  base::UnorderedMap<u64, Tile> tiles_;
  u32 version_counter_ = 0;
};

}  // namespace rx::nav

#endif  // RX_NAV_NAVMESH_H_
