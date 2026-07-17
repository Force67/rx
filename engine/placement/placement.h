#ifndef RX_PLACEMENT_PLACEMENT_H_
#define RX_PLACEMENT_PLACEMENT_H_

#include <compare>
#include <span>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "placement/ecotope.h"
#include "placement/world_data.h"

namespace rx::placement {

// Fixed pipeline dimensions, shared with the HLSL (placement_common.hlsli).
inline constexpr u32 kDensityResolution = 64;   // density texels per tile side
inline constexpr u32 kTileFootprints = 16;      // tile side = footprint * 16
inline constexpr u32 kMaxStackLayers = 8;       // layers per dither stack
inline constexpr u32 kPatternPointCount = 256;  // one dither pattern per tile

// A dither stack: every compiled layer with the same footprint, evaluated
// together so their densities occupy disjoint threshold intervals (layered
// dithering). One stack-tile pair is the unit of GPU work.
struct PlacementStack {
  f32 footprint = 1.0f;
  f32 tile_size = 16.0f;
  f32 stream_radius = 0.0f;  // world meters; tiles beyond this are evicted
  u32 first_layer = 0;       // range into the flat compiled layer array
  u32 layer_count = 0;
};

struct TileKey {
  u32 stack = 0;
  i32 x = 0;
  i32 z = 0;

  bool operator==(const TileKey&) const = default;
  auto operator<=>(const TileKey&) const = default;
};

// One placed object as produced by the PLACEMENT stage (and the CPU
// reference). `layer` indexes the flat compiled layer array; `point` is the
// pattern-point rank - together with the tile coordinate they form the
// stable identity that seeded the instance's variation.
struct PlacedInstance {
  Mat4 transform;
  u32 layer = 0;
  u32 point = 0;
  i32 tile_x = 0;
  i32 tile_z = 0;
};

struct PlacementConfig {
  u32 seed = 0x9E0C17u;
  // Pattern-space jitter (fraction of the footprint) that breaks up the
  // repeated dither structure across tiles.
  f32 jitter = 0.35f;
  // Streaming radius per stack, in tiles (radius_tiles=2 keeps a 5x5 ring).
  u32 radius_tiles = 2;
  // Upper bound on stack-tiles generated per Update; the pipeline batches
  // this many jobs per frame to bound memory and avoid GPU spikes.
  u32 max_jobs_per_update = 12;
};

// The placement system: compiles authored ecotopes into flat layers and
// dither stacks, decides which tiles must exist around the viewer, tracks
// invalidation when WorldData is edited, and provides the CPU reference
// generator that mirrors the GPU pipeline (used headless and by tests).
//
// The caller (demo/game glue) drives it per frame:
//   system.Update(camera_pos);
//   for (TileKey t : system.evicted()) DestroyGroups(t);
//   for (TileKey t : system.pending()) Generate(t);   // GPU or CPU path
// and reports completion with MarkLive/Release.
class RX_PLACEMENT_EXPORT PlacementSystem {
 public:
  PlacementSystem(const WorldData* world, u32 height_map, PlacementConfig config);

  // Appends an ecotope's layers; call Compile() once all are added.
  void AddEcotope(const Ecotope& ecotope);

  // Groups layers into same-footprint stacks. Layer order within a stack
  // follows authoring order; put the most common layers first so their
  // interval test resolves earliest.
  void Compile();

  const WorldData& world() const { return *world_; }
  u32 height_map() const { return height_map_; }
  const PlacementConfig& config() const { return config_; }
  std::span<const PlacementLayer> layers() const { return {layers_.data(), layers_.size()}; }
  std::span<const PlacementStack> stacks() const { return {stacks_.data(), stacks_.size()}; }

  // Streaming bookkeeping. Update() diffs the wanted set around the viewer
  // against live/in-flight tiles and refreshes pending()/evicted().
  void Update(const Vec3& viewer);
  std::span<const TileKey> pending() const { return {pending_.data(), pending_.size()}; }
  std::span<const TileKey> evicted() const { return {evicted_.data(), evicted_.size()}; }
  void MarkInFlight(const TileKey& key);
  void MarkLive(const TileKey& key);
  // Forgets a tile entirely (after its instances are destroyed).
  void Release(const TileKey& key);
  bool IsLive(const TileKey& key) const;

  // Invalidate every tile whose density evaluation may sample the given
  // world rect; live tiles regenerate, unrelated tiles are untouched.
  void InvalidateRegion(f32 min_x, f32 min_z, f32 max_x, f32 max_z);

  // CPU reference generator: bit-stable mirror of the GPU DENSITYMAP +
  // GENERATE + PLACEMENT stages for one stack-tile.
  void EmitTileCpu(const TileKey& key, base::Vector<PlacedInstance>& out) const;

  // World-space min corner of a tile.
  Vec3 TileOrigin(const TileKey& key) const;

 private:
  struct TileState {
    TileKey key;
    u8 state = 0;  // 0 free (pending), 1 in flight, 2 live
    bool stale = false;
  };
  TileState* Find(const TileKey& key);
  const TileState* Find(const TileKey& key) const;

  const WorldData* world_ = nullptr;
  u32 height_map_ = 0;
  PlacementConfig config_;

  base::Vector<PlacementLayer> layers_;
  base::Vector<PlacementStack> stacks_;

  base::Vector<TileState> tiles_;
  base::Vector<TileKey> pending_;
  base::Vector<TileKey> evicted_;
};

}  // namespace rx::placement

#endif  // RX_PLACEMENT_PLACEMENT_H_
