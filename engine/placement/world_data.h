#ifndef RX_PLACEMENT_WORLD_DATA_H_
#define RX_PLACEMENT_WORLD_DATA_H_

#include <span>
#include <string>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"

namespace rx::placement {

// WorldData is the compact shared description of the landscape the density
// programs sample: a set of named single-channel 2D maps (heights, masks,
// topology, painted intent) covering one square world region on a common
// grid. A few bytes per square meter of maps replaces storing every placed
// object, and because programs only read maps, an edit to a map region only
// affects placements that sample it.
//
// All maps share origin/extent/resolution so the GPU side can upload them as
// one texture array with a single uv transform. Values are plain f32; masks
// use 0..1 by convention, the height map is in meters.
class RX_PLACEMENT_EXPORT WorldData {
 public:
  WorldData(f32 origin_x, f32 origin_z, f32 extent_m, u32 resolution);

  // Adds a map filled with `fill` and returns its index (the slot density
  // programs reference).
  u32 AddMap(std::string name, f32 fill = 0.0f);

  u32 map_count() const { return static_cast<u32>(maps_.size()); }
  const std::string& map_name(u32 map) const { return maps_[map].name; }
  u32 resolution() const { return resolution_; }
  f32 origin_x() const { return origin_x_; }
  f32 origin_z() const { return origin_z_; }
  f32 extent() const { return extent_; }
  f32 meters_per_texel() const { return extent_ / static_cast<f32>(resolution_); }

  std::span<f32> texels(u32 map);
  std::span<const f32> texels(u32 map) const;
  f32& At(u32 map, u32 x, u32 z);

  // Bilinear sample at a world position, clamped at the region border.
  // Mirrors the GPU sampler state (linear, clamp-to-edge, texel centers).
  f32 Sample(u32 map, f32 world_x, f32 world_z) const;

  // Fills a map from a callback evaluated at every texel center.
  template <typename Fn>
  void Generate(u32 map, Fn&& fn) {
    f32 step = meters_per_texel();
    for (u32 z = 0; z < resolution_; ++z) {
      for (u32 x = 0; x < resolution_; ++x) {
        f32 wx = origin_x_ + (static_cast<f32>(x) + 0.5f) * step;
        f32 wz = origin_z_ + (static_cast<f32>(z) + 0.5f) * step;
        At(map, x, z) = fn(wx, wz);
      }
    }
    ++revisions_[map];
  }

  // Paints `value` into a disc, lerped by brush strength at the rim; the
  // canonical "artist edits a map, only nearby tiles regenerate" entry point.
  void PaintDisc(u32 map, f32 center_x, f32 center_z, f32 radius, f32 value);

  // Bumped on every edit; the placement system compares revisions to know
  // when the GPU copy and dependent tiles are stale.
  u64 revision(u32 map) const { return revisions_[map]; }
  void MarkEdited(u32 map) { ++revisions_[map]; }

 private:
  f32 origin_x_ = 0;
  f32 origin_z_ = 0;
  f32 extent_ = 0;
  u32 resolution_ = 0;

  struct Map {
    std::string name;
    base::Vector<f32> texels;
  };
  base::Vector<Map> maps_;
  base::Vector<u64> revisions_;
};

}  // namespace rx::placement

#endif  // RX_PLACEMENT_WORLD_DATA_H_
