#include "base/algorithm.h"
#include "base/containers/span.h"
#include "base/memory/move.h"
#include "base/numeric_limits.h"
#include "base/optional.h"
#include "core/scalar.h"
#include "terrain/terrain.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace rx::terrain {
namespace {

bool IsFinite(Vec3 value) {
  return isfinite(value.x) && isfinite(value.y) &&
         isfinite(value.z);
}

bool CheckedSampleCount(u32 quads, size_t *count) {
  if (quads == 0 || quads > 65534)
    return false;
  const size_t side = static_cast<size_t>(quads) + 1;
  if (side > SIZE_MAX / side)
    return false;
  *count = side * side;
  return *count <= base::MinMax<u32>::max();
}

bool KeyLess(TerrainTileKey a, TerrainTileKey b) {
  return a.x != b.x ? a.x < b.x : a.z < b.z;
}

bool ChangeLess(const TerrainSampleChange &a, const TerrainSampleChange &b) {
  if (a.tile != b.tile)
    return KeyLess(a.tile, b.tile);
  return a.sample < b.sample;
}

template <typename T, typename Less>
bool IsSorted(const base::Vector<T> &values, Less less) {
  for (size_t i = 1; i < values.size(); ++i) {
    if (less(values[i], values[i - 1]))
      return false;
  }
  return true;
}

u32 ActiveLayerCount(const TerrainDesc &desc) {
  return static_cast<u32>(rx::Min<size_t>(desc.layers.size(), 4));
}

TerrainWeights NormalizeWeights(TerrainWeights value, u32 layer_count) {
  layer_count = rx::Clamp(layer_count, 1u, 4u);
  u32 total = 0;
  for (u32 i = 0; i < layer_count; ++i)
    total += value.rgba[i];
  for (u32 i = layer_count; i < 4; ++i)
    value.rgba[i] = 0;
  if (total == 0)
    return {};

  TerrainWeights normalized;
  normalized.rgba = {0, 0, 0, 0};
  struct Remainder {
    u32 layer;
    u32 value;
  };
  Remainder remainders[4] = {};
  u32 assigned = 0;
  for (u32 i = 0; i < layer_count; ++i) {
    const u32 scaled = static_cast<u32>(value.rgba[i]) * 255;
    normalized.rgba[i] = static_cast<u8>(scaled / total);
    assigned += normalized.rgba[i];
    remainders[i] = {i, scaled % total};
  }
  // Layers are distinct, so the order is total and any correct sort agrees.
  base::Sort(remainders, remainders + layer_count,
             [](const Remainder &a, const Remainder &b) {
               return a.value != b.value ? a.value > b.value : a.layer < b.layer;
             });
  for (u32 i = 0; assigned < 255; ++i, ++assigned) {
    ++normalized.rgba[remainders[i % layer_count].layer];
  }
  return normalized;
}

bool IsNormalized(TerrainWeights value, u32 layer_count) {
  if (layer_count == 0 || layer_count > 4)
    return false;
  u32 total = 0;
  for (u32 i = 0; i < layer_count; ++i)
    total += value.rgba[i];
  for (u32 i = layer_count; i < 4; ++i) {
    if (value.rgba[i] != 0)
      return false;
  }
  return total == 255;
}

TerrainWeights PaintWeights(TerrainWeights old, u32 target, f32 amount,
                            u32 layer_count) {
  amount = rx::Clamp(amount, 0.0f, 1.0f);
  const u32 old_target = old.rgba[target];
  const u32 new_target = static_cast<u32>(rx::Clamp(
      ::lroundf(old_target + (255.0f - old_target) * amount), 0l, 255l));
  const u32 old_others = 255 - old_target;
  const u32 new_others = 255 - new_target;

  TerrainWeights result;
  result.rgba = {0, 0, 0, 0};
  result.rgba[target] = static_cast<u8>(new_target);
  if (old_others == 0 || new_others == 0)
    return result;

  struct Remainder {
    u32 layer;
    u32 value;
  };
  Remainder remainders[3] = {};
  u32 remainder_count = 0;
  u32 assigned = 0;
  for (u32 i = 0; i < layer_count; ++i) {
    if (i == target)
      continue;
    const u32 scaled = static_cast<u32>(old.rgba[i]) * new_others;
    result.rgba[i] = static_cast<u8>(scaled / old_others);
    assigned += result.rgba[i];
    remainders[remainder_count++] = {i, scaled % old_others};
  }
  auto better = [](const Remainder& a, const Remainder& b) {
    return a.value != b.value ? a.value > b.value : a.layer < b.layer;
  };
  for (u32 i = 0; i < remainder_count; ++i) {
    for (u32 j = i + 1; j < remainder_count; ++j) {
      if (better(remainders[j], remainders[i])) base::Swap(remainders[i], remainders[j]);
    }
  }
  for (u32 i = 0; assigned < new_others; ++i, ++assigned) {
    ++result.rgba[remainders[i % remainder_count].layer];
  }
  return result;
}

i64 FloorDiv(i64 value, i64 divisor) {
  i64 quotient = value / divisor;
  const i64 remainder = value % divisor;
  if (remainder < 0)
    --quotient;
  return quotient;
}

i64 PositiveMod(i64 value, i64 divisor) {
  const i64 remainder = value % divisor;
  return remainder < 0 ? remainder + divisor : remainder;
}

u32 SampleIndex(u32 side, u32 x, u32 z) { return z * side + x; }

void HashByte(u64 *hash, u8 value) {
  *hash ^= value;
  *hash *= 0x100000001b3ull;
}

void HashU32(u64 *hash, u32 value) {
  for (u32 shift = 0; shift < 32; shift += 8)
    HashByte(hash, static_cast<u8>(value >> shift));
}

void HashU64(u64 *hash, u64 value) {
  for (u32 shift = 0; shift < 64; shift += 8)
    HashByte(hash, static_cast<u8>(value >> shift));
}

u32 PackDebugColor(const TerrainDesc &desc, TerrainWeights weights) {
  u32 channels[4] = {};
  for (u32 layer = 0; layer < ActiveLayerCount(desc); ++layer) {
    for (u32 channel = 0; channel < 4; ++channel) {
      channels[channel] += static_cast<u32>(weights.rgba[layer]) *
                           desc.layers[layer].debug_rgba[channel];
    }
  }
  for (u32 &channel : channels)
    channel = (channel + 127) / 255;
  return channels[0] | channels[1] << 8 | channels[2] << 16 | channels[3] << 24;
}

bool RayBounds(Vec3 origin, Vec3 direction,
               const scene::WorldStreamRegion &bounds, f32 maximum_distance,
               f32 *entry) {
  double minimum_t = 0;
  double maximum_t = maximum_distance;
  const f32 origins[3] = {origin.x, origin.y, origin.z};
  const f32 directions[3] = {direction.x, direction.y, direction.z};
  const f32 minima[3] = {bounds.minimum.x, bounds.minimum.y, bounds.minimum.z};
  const f32 maxima[3] = {bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
  for (u32 axis = 0; axis < 3; ++axis) {
    if (::fabsf(directions[axis]) < 1e-12f) {
      if (origins[axis] < minima[axis] || origins[axis] > maxima[axis])
        return false;
      continue;
    }
    double a =
        (static_cast<double>(minima[axis]) - origins[axis]) / directions[axis];
    double b =
        (static_cast<double>(maxima[axis]) - origins[axis]) / directions[axis];
    if (a > b)
      base::Swap(a, b);
    minimum_t = rx::Max(minimum_t, a);
    maximum_t = rx::Min(maximum_t, b);
    if (minimum_t > maximum_t)
      return false;
  }
  *entry = static_cast<f32>(minimum_t);
  return true;
}

bool RayTriangle(Vec3 origin, Vec3 direction, Vec3 a, Vec3 b, Vec3 c,
                 f32 maximum_distance, f32 *distance, Vec3 *normal) {
  const Vec3 edge_a = b - a;
  const Vec3 edge_b = c - a;
  const Vec3 p = Cross(direction, edge_b);
  const f32 determinant = Dot(edge_a, p);
  if (!isfinite(determinant) || ::fabsf(determinant) < 1e-8f)
    return false;
  const f32 inverse = 1.0f / determinant;
  const Vec3 relative = origin - a;
  const f32 u = Dot(relative, p) * inverse;
  if (!isfinite(u) || u < 0 || u > 1)
    return false;
  const Vec3 q = Cross(relative, edge_a);
  const f32 v = Dot(direction, q) * inverse;
  if (!isfinite(v) || v < 0 || u + v > 1)
    return false;
  const f32 hit = Dot(edge_b, q) * inverse;
  if (!isfinite(hit) || hit < 0 || hit > maximum_distance)
    return false;
  *distance = hit;
  *normal = Normalize(Cross(edge_a, edge_b));
  if (normal->y < 0)
    *normal = *normal * -1.0f;
  return true;
}

} // namespace

Terrain::Terrain() : Terrain(TerrainDesc{}) {}

Terrain::Terrain(TerrainDesc desc) : desc_(base::move(desc)) {
  if (desc_.tile_quads == 0)
    desc_.tile_quads = 32;
  if (!isfinite(desc_.sample_spacing) || desc_.sample_spacing <= 0) {
    desc_.sample_spacing = 1.0f;
  }
  if (!isfinite(desc_.origin.x))
    desc_.origin.x = 0;
  if (!isfinite(desc_.origin.y))
    desc_.origin.y = 0;
  if (!isfinite(desc_.origin.z))
    desc_.origin.z = 0;
  if (desc_.layers.empty())
    desc_.layers.push_back(TerrainLayer{"Layer 0"});
  if (desc_.layers.size() > 4)
    desc_.layers.resize(4);
}

const TerrainTile *Terrain::FindTile(TerrainTileKey key) const {
  const auto found =
      base::LowerBound(tiles_.begin(), tiles_.end(), key,
                       [](const TerrainTile &tile, TerrainTileKey wanted) {
                         return KeyLess(tile.key, wanted);
                       });
  return found != tiles_.end() && found->key == key ? found : nullptr;
}

TerrainTile *Terrain::FindTileMutable(TerrainTileKey key) {
  auto found =
      base::LowerBound(tiles_.begin(), tiles_.end(), key,
                       [](const TerrainTile &tile, TerrainTileKey wanted) {
                         return KeyLess(tile.key, wanted);
                       });
  return found != tiles_.end() && found->key == key ? found : nullptr;
}

void Terrain::RecalculateBounds(TerrainTile *tile) {
  if (!tile || tile->heights.empty())
    return;
  // std::minmax_element's picks: the first smallest and the last largest.
  const f32 *minimum = tile->heights.begin();
  const f32 *maximum = tile->heights.begin();
  for (const f32 *it = tile->heights.begin() + 1; it != tile->heights.end();
       ++it) {
    if (*it < *minimum)
      minimum = it;
    if (!(*it < *maximum))
      maximum = it;
  }
  tile->minimum_height = *minimum;
  tile->maximum_height = *maximum;
}

bool Terrain::AddOrReplaceTile(TerrainTileKey key, base::Span<const f32> heights,
                               base::Span<const TerrainWeights> weights) {
  size_t sample_count = 0;
  if (!CheckedSampleCount(desc_.tile_quads, &sample_count) ||
      heights.size() != sample_count ||
      (!weights.empty() && weights.size() != sample_count) ||
      !isfinite(desc_.tile_quads * desc_.sample_spacing)) {
    return false;
  }
  for (f32 height : heights) {
    if (!isfinite(height))
      return false;
  }

  TerrainTile replacement;
  replacement.key = key;
  replacement.heights.assign(heights.begin(), heights.end());
  replacement.weights.resize(sample_count);
  const u32 layer_count = ActiveLayerCount(desc_);
  if (!weights.empty()) {
    for (size_t i = 0; i < sample_count; ++i) {
      replacement.weights[i] = NormalizeWeights(weights[i], layer_count);
    }
  }
  RecalculateBounds(&replacement);
  if (!isfinite(replacement.maximum_height - replacement.minimum_height))
    return false;

  auto insertion =
      base::LowerBound(tiles_.begin(), tiles_.end(), key,
                       [](const TerrainTile &tile, TerrainTileKey wanted) {
                         return KeyLess(tile.key, wanted);
                       });
  if (insertion != tiles_.end() && insertion->key == key) {
    replacement.revision = insertion->revision + 1;
    *insertion = base::move(replacement);
  } else {
    replacement.revision = 1;
    insertion = tiles_.insert(insertion, base::move(replacement));
  }

  TerrainTile *source = &*insertion;
  const u32 quads = desc_.tile_quads;
  const u32 side = quads + 1;
  auto synchronize = [&](TerrainTileKey neighbor_key, bool normal_dependent,
                         auto copy_samples) {
    TerrainTile *neighbor = FindTileMutable(neighbor_key);
    if (!neighbor)
      return;
    bool changed = false;
    auto copy = [&](u32 source_x, u32 source_z, u32 neighbor_x,
                    u32 neighbor_z) {
      const u32 source_index = SampleIndex(side, source_x, source_z);
      const u32 neighbor_index = SampleIndex(side, neighbor_x, neighbor_z);
      if (neighbor->heights[neighbor_index] != source->heights[source_index] ||
          neighbor->weights[neighbor_index] != source->weights[source_index]) {
        neighbor->heights[neighbor_index] = source->heights[source_index];
        neighbor->weights[neighbor_index] = source->weights[source_index];
        changed = true;
      }
    };
    copy_samples(copy);
    if (changed) {
      RecalculateBounds(neighbor);
    }
    if (changed || normal_dependent)
      ++neighbor->revision;
  };
  auto synchronize_offset = [&](i32 offset_x, i32 offset_z,
                                bool normal_dependent, auto copy_samples) {
    const i64 neighbor_x = static_cast<i64>(key.x) + offset_x;
    const i64 neighbor_z = static_cast<i64>(key.z) + offset_z;
    if (neighbor_x < base::MinMax<i32>::min() ||
        neighbor_x > base::MinMax<i32>::max() ||
        neighbor_z < base::MinMax<i32>::min() ||
        neighbor_z > base::MinMax<i32>::max()) {
      return;
    }
    synchronize({static_cast<i32>(neighbor_x), static_cast<i32>(neighbor_z)},
                normal_dependent, copy_samples);
  };

  synchronize_offset(-1, 0, true, [&](auto copy) {
    for (u32 z = 0; z <= quads; ++z)
      copy(0, z, quads, z);
  });
  synchronize_offset(1, 0, true, [&](auto copy) {
    for (u32 z = 0; z <= quads; ++z)
      copy(quads, z, 0, z);
  });
  synchronize_offset(0, -1, true, [&](auto copy) {
    for (u32 x = 0; x <= quads; ++x)
      copy(x, 0, x, quads);
  });
  synchronize_offset(0, 1, true, [&](auto copy) {
    for (u32 x = 0; x <= quads; ++x)
      copy(x, quads, x, 0);
  });
  // Diagonals are normal-dependent too: the corner vertex normal reads the
  // new tile's edge-adjacent samples through GridHeight, so it can change
  // even when the single shared corner sample keeps its value.
  synchronize_offset(-1, -1, true,
                     [&](auto copy) { copy(0, 0, quads, quads); });
  synchronize_offset(1, -1, true,
                     [&](auto copy) { copy(quads, 0, 0, quads); });
  synchronize_offset(-1, 1, true,
                     [&](auto copy) { copy(0, quads, quads, 0); });
  synchronize_offset(1, 1, true,
                     [&](auto copy) { copy(quads, quads, 0, 0); });
  return true;
}

base::Optional<f32> Terrain::GridHeight(i64 grid_x, i64 grid_z) const {
  const i64 quads = desc_.tile_quads;
  const i64 base_x = FloorDiv(grid_x, quads);
  const i64 base_z = FloorDiv(grid_z, quads);
  const i64 remainder_x = PositiveMod(grid_x, quads);
  const i64 remainder_z = PositiveMod(grid_z, quads);
  const i64 candidate_x[2] = {base_x, base_x - 1};
  const i64 candidate_z[2] = {base_z, base_z - 1};
  const u32 count_x = remainder_x == 0 ? 2 : 1;
  const u32 count_z = remainder_z == 0 ? 2 : 1;
  const u32 side = desc_.tile_quads + 1;
  for (u32 z = 0; z < count_z; ++z) {
    for (u32 x = 0; x < count_x; ++x) {
      if (candidate_x[x] < base::MinMax<i32>::min() ||
          candidate_x[x] > base::MinMax<i32>::max() ||
          candidate_z[z] < base::MinMax<i32>::min() ||
          candidate_z[z] > base::MinMax<i32>::max()) {
        continue;
      }
      const TerrainTile *tile = FindTile(
          {static_cast<i32>(candidate_x[x]), static_cast<i32>(candidate_z[z])});
      if (!tile)
        continue;
      const u32 local_x =
          x == 0 ? static_cast<u32>(remainder_x) : desc_.tile_quads;
      const u32 local_z =
          z == 0 ? static_cast<u32>(remainder_z) : desc_.tile_quads;
      return tile->heights[SampleIndex(side, local_x, local_z)];
    }
  }
  return base::nullopt;
}

base::Optional<f32> Terrain::SampleHeight(f32 world_x, f32 world_z) const {
  if (!isfinite(world_x) || !isfinite(world_z) || tiles_.empty())
    return base::nullopt;
  const double grid_x =
      (static_cast<double>(world_x) - desc_.origin.x) / desc_.sample_spacing;
  const double grid_z =
      (static_cast<double>(world_z) - desc_.origin.z) / desc_.sample_spacing;
  const double quads = desc_.tile_quads;
  const double tile_floor_x = ::floor(grid_x / quads);
  const double tile_floor_z = ::floor(grid_z / quads);
  if (tile_floor_x <=
          static_cast<double>(base::MinMax<i64>::min()) + 1 ||
      tile_floor_x >=
          static_cast<double>(base::MinMax<i64>::max()) - 1 ||
      tile_floor_z <=
          static_cast<double>(base::MinMax<i64>::min()) + 1 ||
      tile_floor_z >=
          static_cast<double>(base::MinMax<i64>::max()) - 1) {
    return base::nullopt;
  }
  const i64 base_x = static_cast<i64>(tile_floor_x);
  const i64 base_z = static_cast<i64>(tile_floor_z);
  const double edge_x = grid_x - static_cast<double>(base_x) * quads;
  const double edge_z = grid_z - static_cast<double>(base_z) * quads;
  constexpr double kBoundaryEpsilon = 1e-6;
  const i64 candidate_x[2] = {base_x, base_x - 1};
  const i64 candidate_z[2] = {base_z, base_z - 1};
  const u32 count_x = ::fabs(edge_x) <= kBoundaryEpsilon ? 2 : 1;
  const u32 count_z = ::fabs(edge_z) <= kBoundaryEpsilon ? 2 : 1;
  const u32 side = desc_.tile_quads + 1;

  for (u32 cz = 0; cz < count_z; ++cz) {
    for (u32 cx = 0; cx < count_x; ++cx) {
      const i64 tile_x = candidate_x[cx];
      const i64 tile_z = candidate_z[cz];
      if (tile_x < base::MinMax<i32>::min() ||
          tile_x > base::MinMax<i32>::max() ||
          tile_z < base::MinMax<i32>::min() ||
          tile_z > base::MinMax<i32>::max()) {
        continue;
      }
      const TerrainTile *tile =
          FindTile({static_cast<i32>(tile_x), static_cast<i32>(tile_z)});
      if (!tile)
        continue;
      double local_x = grid_x - static_cast<double>(tile_x) * quads;
      double local_z = grid_z - static_cast<double>(tile_z) * quads;
      if (local_x < -kBoundaryEpsilon || local_x > quads + kBoundaryEpsilon ||
          local_z < -kBoundaryEpsilon || local_z > quads + kBoundaryEpsilon) {
        continue;
      }
      local_x = rx::Clamp(local_x, 0.0, quads);
      local_z = rx::Clamp(local_z, 0.0, quads);
      const u32 cell_x =
          rx::Min(static_cast<u32>(::floor(local_x)), desc_.tile_quads - 1);
      const u32 cell_z =
          rx::Min(static_cast<u32>(::floor(local_z)), desc_.tile_quads - 1);
      const f32 fraction_x = static_cast<f32>(local_x - cell_x);
      const f32 fraction_z = static_cast<f32>(local_z - cell_z);
      const f32 a = tile->heights[SampleIndex(side, cell_x, cell_z)];
      const f32 b = tile->heights[SampleIndex(side, cell_x + 1, cell_z)];
      const f32 c = tile->heights[SampleIndex(side, cell_x, cell_z + 1)];
      const f32 d = tile->heights[SampleIndex(side, cell_x + 1, cell_z + 1)];
      const f32 height = fraction_x + fraction_z <= 1.0f
                             ? a + fraction_x * (b - a) + fraction_z * (c - a)
                             : d + (1.0f - fraction_x) * (c - d) +
                                   (1.0f - fraction_z) * (b - d);
      const double world_height = static_cast<double>(desc_.origin.y) + height;
      if (!isfinite(world_height) ||
          ::fabs(world_height) > FLT_MAX) {
        return base::nullopt;
      }
      return static_cast<f32>(world_height);
    }
  }
  return base::nullopt;
}

asset::AssetId Terrain::TileAssetId(TerrainTileKey key) const {
  if (!desc_.id) return {};
  u64 hash = 0xcbf29ce484222325ull;
  constexpr char tag[] = "rx.terrain.tile";
  for (char c : tag)
    HashByte(&hash, static_cast<u8>(c));
  HashU64(&hash, desc_.id.hash);
  HashU32(&hash, rx::BitCast<u32>(key.x));
  HashU32(&hash, rx::BitCast<u32>(key.z));
  return asset::AssetId{hash == 0 ? 1 : hash};
}

base::Optional<scene::WorldStreamRegion>
Terrain::TileRegion(TerrainTileKey key, u32 channels, i32 priority) const {
  if (!desc_.id) return base::nullopt;
  const TerrainTile *tile = FindTile(key);
  if (!tile)
    return base::nullopt;
  const double width =
      static_cast<double>(desc_.tile_quads) * desc_.sample_spacing;
  const double minimum_x = desc_.origin.x + static_cast<double>(key.x) * width;
  const double minimum_z = desc_.origin.z + static_cast<double>(key.z) * width;
  const double maximum_x = minimum_x + width;
  const double maximum_z = minimum_z + width;
  const double minimum_y =
      static_cast<double>(desc_.origin.y) + tile->minimum_height;
  const double maximum_y =
      static_cast<double>(desc_.origin.y) + tile->maximum_height;
  const double maximum_float = FLT_MAX;
  const double values[] = {minimum_x, minimum_y, minimum_z,
                           maximum_x, maximum_y, maximum_z};
  for (double value : values) {
    if (!isfinite(value) || value < -maximum_float ||
        value > maximum_float) {
      return base::nullopt;
    }
  }
  return scene::WorldStreamRegion{
      TileAssetId(key).hash,
      {static_cast<f32>(minimum_x), static_cast<f32>(minimum_y),
       static_cast<f32>(minimum_z)},
      {static_cast<f32>(maximum_x), static_cast<f32>(maximum_y),
       static_cast<f32>(maximum_z)},
      priority,
      channels};
}

void Terrain::GatherStreamRegions(
    const scene::WorldStreamQuery &query,
    base::Vector<scene::WorldStreamRegion> *regions, u32 channels,
    i32 priority) const {
  if (!regions)
    return;
  regions->clear();
  if ((query.channels & channels) == 0 || !isfinite(query.radius) ||
      query.radius < 0)
    return;
  u8 axes = query.axes & scene::kWorldStreamXYZ;
  if (axes == 0)
    axes = scene::kWorldStreamXYZ;
  auto valid_axis = [&](u8 axis, f32 origin, f32 predicted) {
    return (axes & axis) == 0 ||
           (isfinite(origin) && isfinite(predicted));
  };
  if (!valid_axis(scene::kWorldStreamX, query.origin.x, query.predicted.x) ||
      !valid_axis(scene::kWorldStreamY, query.origin.y, query.predicted.y) ||
      !valid_axis(scene::kWorldStreamZ, query.origin.z, query.predicted.z)) {
    return;
  }

  auto intersects_axis = [&](u8 axis, f32 start, f32 end, f32 minimum,
                             f32 maximum) {
    if ((axes & axis) == 0)
      return true;
    const double query_minimum =
        rx::Min(start, end) - static_cast<double>(query.radius);
    const double query_maximum =
        rx::Max(start, end) + static_cast<double>(query.radius);
    return maximum >= query_minimum && minimum <= query_maximum;
  };
  for (const TerrainTile &tile : tiles_) {
    const base::Optional<scene::WorldStreamRegion> region =
        TileRegion(tile.key, channels, priority);
    if (!region)
      continue;
    if (intersects_axis(scene::kWorldStreamX, query.origin.x, query.predicted.x,
                        region->minimum.x, region->maximum.x) &&
        intersects_axis(scene::kWorldStreamY, query.origin.y, query.predicted.y,
                        region->minimum.y, region->maximum.y) &&
        intersects_axis(scene::kWorldStreamZ, query.origin.z, query.predicted.z,
                        region->minimum.z, region->maximum.z)) {
      regions->push_back(*region);
    }
  }
}

base::Optional<asset::Mesh>
Terrain::BuildTileMesh(TerrainTileKey key, asset::AssetId material) const {
  const TerrainTile *tile = FindTile(key);
  if (!tile)
    return base::nullopt;
  const u32 quads = desc_.tile_quads;
  const u32 side = quads + 1;
  const f32 width = static_cast<f32>(quads) * desc_.sample_spacing;
  if (!isfinite(width))
    return base::nullopt;

  asset::Mesh mesh;
  mesh.id = TileAssetId(key);
  mesh.dynamic_vertices = true;
  asset::MeshLod &lod = mesh.lods.emplace_back();
  lod.vertices.resize(tile->heights.size());
  lod.indices.reserve(static_cast<size_t>(quads) * quads * 6);

  for (u32 z = 0; z <= quads; ++z) {
    for (u32 x = 0; x <= quads; ++x) {
      const u32 index = SampleIndex(side, x, z);
      const f32 center = tile->heights[index];
      const i64 grid_x = static_cast<i64>(key.x) * quads + x;
      const i64 grid_z = static_cast<i64>(key.z) * quads + z;
      const base::Optional<f32> left = GridHeight(grid_x - 1, grid_z);
      const base::Optional<f32> right = GridHeight(grid_x + 1, grid_z);
      const base::Optional<f32> down = GridHeight(grid_x, grid_z - 1);
      const base::Optional<f32> up = GridHeight(grid_x, grid_z + 1);
      const f32 dx = left && right
                         ? (*right - *left) / (2 * desc_.sample_spacing)
                     : right ? (*right - center) / desc_.sample_spacing
                     : left  ? (center - *left) / desc_.sample_spacing
                             : 0;
      const f32 dz = down && up ? (*up - *down) / (2 * desc_.sample_spacing)
                     : up       ? (*up - center) / desc_.sample_spacing
                     : down     ? (center - *down) / desc_.sample_spacing
                                : 0;
      const Vec3 normal = Normalize(Vec3{-dx, 1, -dz});
      const Vec3 tangent = Normalize(Vec3{1, dx, 0});
      asset::Vertex &vertex = lod.vertices[index];
      vertex.position[0] = static_cast<f32>(x) * desc_.sample_spacing;
      vertex.position[1] = center;
      vertex.position[2] = static_cast<f32>(z) * desc_.sample_spacing;
      vertex.normal[0] = normal.x;
      vertex.normal[1] = normal.y;
      vertex.normal[2] = normal.z;
      vertex.tangent[0] = tangent.x;
      vertex.tangent[1] = tangent.y;
      vertex.tangent[2] = tangent.z;
      vertex.tangent[3] = -1;
      vertex.uv[0] = static_cast<f32>(x) / quads;
      vertex.uv[1] = static_cast<f32>(z) / quads;
      vertex.color = PackDebugColor(desc_, tile->weights[index]);
    }
  }
  for (u32 z = 0; z < quads; ++z) {
    for (u32 x = 0; x < quads; ++x) {
      const u32 a = SampleIndex(side, x, z);
      const u32 b = SampleIndex(side, x + 1, z);
      const u32 c = SampleIndex(side, x, z + 1);
      const u32 d = SampleIndex(side, x + 1, z + 1);
      lod.indices.push_back(a);
      lod.indices.push_back(c);
      lod.indices.push_back(b);
      lod.indices.push_back(b);
      lod.indices.push_back(c);
      lod.indices.push_back(d);
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
  mesh.bounds_center[0] = width * 0.5f;
  mesh.bounds_center[1] = tile->minimum_height +
                          (tile->maximum_height - tile->minimum_height) * 0.5f;
  mesh.bounds_center[2] = width * 0.5f;
  const f32 half_height = (tile->maximum_height - tile->minimum_height) * 0.5f;
  mesh.bounds_radius = rx::Hypot(width * 0.5f, width * 0.5f, half_height);
  return mesh;
}

base::Optional<TerrainRayHit> Terrain::Raycast(Vec3 origin, Vec3 direction,
                                              f32 maximum_distance) const {
  if (!IsFinite(origin) || !IsFinite(direction) ||
      ::isnan(maximum_distance) || maximum_distance < 0) {
    return base::nullopt;
  }
  const double length = rx::Hypot(direction.x, direction.y, direction.z);
  if (!isfinite(length) || length <= 0)
    return base::nullopt;
  direction = {static_cast<f32>(static_cast<double>(direction.x) / length),
               static_cast<f32>(static_cast<double>(direction.y) / length),
               static_cast<f32>(static_cast<double>(direction.z) / length)};
  if (!IsFinite(direction))
    return base::nullopt;

  f32 nearest = maximum_distance;
  base::Optional<TerrainRayHit> result;
  const u32 quads = desc_.tile_quads;
  const u32 side = quads + 1;
  for (const TerrainTile &tile : tiles_) {
    const double width = static_cast<double>(quads) * desc_.sample_spacing;
    const double minimum_x =
        desc_.origin.x + static_cast<double>(tile.key.x) * width;
    const double minimum_z =
        desc_.origin.z + static_cast<double>(tile.key.z) * width;
    const double minimum_y =
        static_cast<double>(desc_.origin.y) + tile.minimum_height;
    const double maximum_y =
        static_cast<double>(desc_.origin.y) + tile.maximum_height;
    const double values[] = {minimum_x, minimum_y, minimum_z,
                             minimum_x + width, maximum_y, minimum_z + width};
    bool valid_bounds = true;
    for (double value : values) {
      if (!isfinite(value) ||
          ::fabs(value) > FLT_MAX) {
        valid_bounds = false;
        break;
      }
    }
    if (!valid_bounds)
      continue;
    const scene::WorldStreamRegion bounds{
        0,
        {static_cast<f32>(values[0]), static_cast<f32>(values[1]),
         static_cast<f32>(values[2])},
        {static_cast<f32>(values[3]), static_cast<f32>(values[4]),
         static_cast<f32>(values[5])}};
    f32 entry = 0;
    if (!RayBounds(origin, direction, bounds, nearest, &entry) ||
        entry > nearest)
      continue;
    const f32 world_x = bounds.minimum.x;
    const f32 world_z = bounds.minimum.z;
    for (u32 z = 0; z < quads; ++z) {
      for (u32 x = 0; x < quads; ++x) {
        auto point = [&](u32 sample_x, u32 sample_z) {
          return Vec3{world_x + sample_x * desc_.sample_spacing,
                      desc_.origin.y +
                          tile.heights[SampleIndex(side, sample_x, sample_z)],
                      world_z + sample_z * desc_.sample_spacing};
        };
        const Vec3 a = point(x, z);
        const Vec3 b = point(x + 1, z);
        const Vec3 c = point(x, z + 1);
        const Vec3 d = point(x + 1, z + 1);
        f32 distance = 0;
        Vec3 normal;
        if (RayTriangle(origin, direction, a, c, b, nearest, &distance, &normal)) {
          nearest = distance;
          result = TerrainRayHit{origin + direction * distance, normal,
                                 tile.key, distance};
        }
        if (RayTriangle(origin, direction, b, c, d, nearest, &distance, &normal)) {
          nearest = distance;
          result = TerrainRayHit{origin + direction * distance, normal,
                                 tile.key, distance};
        }
      }
    }
  }
  return result;
}

TerrainChange Terrain::ApplyBrush(const TerrainBrush &brush) {
  TerrainChange change;
  change.terrain = desc_.id;
  if (!isfinite(brush.center_x) || !isfinite(brush.center_z) ||
      !isfinite(brush.radius) || brush.radius <= 0 ||
      !isfinite(brush.strength) || brush.strength <= 0 ||
      !isfinite(brush.falloff) || brush.falloff < 0 ||
      (brush.mode == TerrainBrushMode::kFlatten &&
       !isfinite(brush.flatten_target)) ||
      (brush.mode == TerrainBrushMode::kPaintLayer &&
       brush.layer >= ActiveLayerCount(desc_))) {
    return change;
  }

  const u32 quads = desc_.tile_quads;
  const u32 side = quads + 1;
  const double tile_width = static_cast<double>(quads) * desc_.sample_spacing;
  for (const TerrainTile &tile : tiles_) {
    const double tile_x =
        desc_.origin.x + static_cast<double>(tile.key.x) * tile_width;
    const double tile_z =
        desc_.origin.z + static_cast<double>(tile.key.z) * tile_width;
    if (brush.center_x + brush.radius < tile_x ||
        brush.center_x - brush.radius > tile_x + tile_width ||
        brush.center_z + brush.radius < tile_z ||
        brush.center_z - brush.radius > tile_z + tile_width) {
      continue;
    }
    const size_t first_change = change.samples.size();
    for (u32 z = 0; z <= quads; ++z) {
      for (u32 x = 0; x <= quads; ++x) {
        const i64 grid_x = static_cast<i64>(tile.key.x) * quads + x;
        const i64 grid_z = static_cast<i64>(tile.key.z) * quads + z;
        const double world_x =
            desc_.origin.x + static_cast<double>(grid_x) * desc_.sample_spacing;
        const double world_z =
            desc_.origin.z + static_cast<double>(grid_z) * desc_.sample_spacing;
        const f32 distance = static_cast<f32>(
            ::hypot(world_x - brush.center_x, world_z - brush.center_z));
        if (distance > brush.radius)
          continue;
        const f32 radial = rx::Max(0.0f, 1.0f - distance / brush.radius);
        const f32 influence =
            brush.falloff == 0 ? 1.0f : ::powf(radial, brush.falloff);
        if (influence <= 0)
          continue;

        const u32 index = SampleIndex(side, x, z);
        TerrainSampleState old_value{tile.heights[index], tile.weights[index]};
        TerrainSampleState new_value = old_value;
        switch (brush.mode) {
        case TerrainBrushMode::kRaise:
          new_value.height += brush.strength * influence;
          break;
        case TerrainBrushMode::kLower:
          new_value.height -= brush.strength * influence;
          break;
        case TerrainBrushMode::kSmooth: {
          const base::Optional<f32> neighbors[] = {
              GridHeight(grid_x - 1, grid_z), GridHeight(grid_x + 1, grid_z),
              GridHeight(grid_x, grid_z - 1), GridHeight(grid_x, grid_z + 1)};
          f32 total = 0;
          u32 count = 0;
          for (base::Optional<f32> neighbor : neighbors) {
            if (neighbor) {
              total += *neighbor;
              ++count;
            }
          }
          if (count > 0) {
            const f32 amount =
                rx::Clamp(brush.strength * influence, 0.0f, 1.0f);
            new_value.height += (total / count - old_value.height) * amount;
          }
          break;
        }
        case TerrainBrushMode::kFlatten: {
          const f32 amount = rx::Clamp(brush.strength * influence, 0.0f, 1.0f);
          const f32 target = brush.flatten_target - desc_.origin.y;
          new_value.height += (target - old_value.height) * amount;
          break;
        }
        case TerrainBrushMode::kPaintLayer:
          new_value.weights =
              PaintWeights(old_value.weights, brush.layer,
                           rx::Clamp(brush.strength * influence, 0.0f, 1.0f),
                           ActiveLayerCount(desc_));
          break;
        }
        if (!isfinite(new_value.height) || new_value == old_value)
          continue;
        change.samples.push_back({tile.key, index, old_value, new_value});
      }
    }
    if (change.samples.size() != first_change)
      change.dirty_tiles.push_back(tile.key);
  }
  // Central-difference normals on a neighboring tile's border depend on the
  // first interior sample of this tile. Include those derived-data dependents
  // even though none of their authored samples changed.
  base::Vector<TerrainTileKey> normal_dependents;
  auto neighbor = [&](TerrainTileKey key, i32 dx,
                      i32 dz) -> base::Optional<TerrainTileKey> {
    const i64 x = static_cast<i64>(key.x) + dx;
    const i64 z = static_cast<i64>(key.z) + dz;
    if (x < base::MinMax<i32>::min() ||
        x > base::MinMax<i32>::max() ||
        z < base::MinMax<i32>::min() ||
        z > base::MinMax<i32>::max()) {
      return base::nullopt;
    }
    return TerrainTileKey{static_cast<i32>(x), static_cast<i32>(z)};
  };
  for (const TerrainSampleChange& sample : change.samples) {
    const u32 local_x = sample.sample % side;
    const u32 local_z = sample.sample / side;
    const base::Optional<TerrainTileKey> west = neighbor(sample.tile, -1, 0);
    const base::Optional<TerrainTileKey> east = neighbor(sample.tile, 1, 0);
    const base::Optional<TerrainTileKey> south = neighbor(sample.tile, 0, -1);
    const base::Optional<TerrainTileKey> north = neighbor(sample.tile, 0, 1);
    if (local_x == 1 && west && FindTile(*west)) normal_dependents.push_back(*west);
    if (local_x + 1 == quads && east && FindTile(*east)) normal_dependents.push_back(*east);
    if (local_z == 1 && south && FindTile(*south)) normal_dependents.push_back(*south);
    if (local_z + 1 == quads && north && FindTile(*north)) normal_dependents.push_back(*north);
    // A diagonal neighbor's corner normal reads this tile's two edge-adjacent
    // samples through GridHeight. When the shared cardinal tile exists its own
    // border copy reaches the diagonal through the pushes above, but in sparse
    // layouts (corner tile present, edge tile missing) that indirection breaks,
    // so mark the diagonal directly.
    const base::Optional<TerrainTileKey> southwest = neighbor(sample.tile, -1, -1);
    const base::Optional<TerrainTileKey> southeast = neighbor(sample.tile, 1, -1);
    const base::Optional<TerrainTileKey> northwest = neighbor(sample.tile, -1, 1);
    const base::Optional<TerrainTileKey> northeast = neighbor(sample.tile, 1, 1);
    if (((local_x == 1 && local_z == 0) || (local_x == 0 && local_z == 1)) &&
        southwest && FindTile(*southwest))
      normal_dependents.push_back(*southwest);
    if (((local_x + 1 == quads && local_z == 0) ||
         (local_x == quads && local_z == 1)) &&
        southeast && FindTile(*southeast))
      normal_dependents.push_back(*southeast);
    if (((local_x == 1 && local_z == quads) ||
         (local_x == 0 && local_z + 1 == quads)) &&
        northwest && FindTile(*northwest))
      normal_dependents.push_back(*northwest);
    if (((local_x + 1 == quads && local_z == quads) ||
         (local_x == quads && local_z + 1 == quads)) &&
        northeast && FindTile(*northeast))
      normal_dependents.push_back(*northeast);
  }
  for (TerrainTileKey key : normal_dependents) change.dirty_tiles.push_back(key);
  // A key is all its value, so equal keys are identical and any correct sort
  // agrees.
  base::Sort(change.dirty_tiles.begin(), change.dirty_tiles.end(), KeyLess);
  change.dirty_tiles.erase(
      base::Unique(change.dirty_tiles.begin(), change.dirty_tiles.end()),
      change.dirty_tiles.end());
  if (!change.empty() && !ApplyChange(change))
    return TerrainChange{};
  return change;
}

bool Terrain::SetChangeState(const TerrainChange &change, bool use_new) {
  if (change.terrain != desc_.id)
    return false;
  const u32 layer_count = ActiveLayerCount(desc_);
  TerrainSampleChange previous;
  bool has_previous = false;
  for (size_t i = 0; i < change.dirty_tiles.size(); ++i) {
    if ((i > 0 && !KeyLess(change.dirty_tiles[i - 1], change.dirty_tiles[i])) ||
        !FindTile(change.dirty_tiles[i])) {
      return false;
    }
  }
  for (const TerrainSampleChange &sample : change.samples) {
    if (has_previous && !ChangeLess(previous, sample))
      return false;
    previous = sample;
    has_previous = true;
    TerrainTile *tile = FindTileMutable(sample.tile);
    const TerrainTileKey *dirty =
        base::LowerBound(change.dirty_tiles.begin(), change.dirty_tiles.end(),
                         sample.tile, KeyLess);
    if (!tile || sample.sample >= tile->heights.size() ||
        dirty == change.dirty_tiles.end() || KeyLess(sample.tile, *dirty)) {
      return false;
    }
    const TerrainSampleState &wanted =
        use_new ? sample.new_value : sample.old_value;
    const TerrainSampleState &expected =
        use_new ? sample.old_value : sample.new_value;
    if (!isfinite(wanted.height) ||
        !IsNormalized(wanted.weights, layer_count) ||
        tile->heights[sample.sample] != expected.height ||
        tile->weights[sample.sample] != expected.weights) {
      return false;
    }
  }

  for (const TerrainSampleChange &sample : change.samples) {
    TerrainTile *tile = FindTileMutable(sample.tile);
    const TerrainSampleState &value =
        use_new ? sample.new_value : sample.old_value;
    tile->heights[sample.sample] = value.height;
    tile->weights[sample.sample] = value.weights;
  }
  for (TerrainTileKey key : change.dirty_tiles) {
    TerrainTile *tile = FindTileMutable(key);
    RecalculateBounds(tile);
    ++tile->revision;
  }
  return true;
}

bool Terrain::ApplyChange(const TerrainChange &change) {
  return SetChangeState(change, true);
}

bool Terrain::RevertChange(const TerrainChange &change) {
  return SetChangeState(change, false);
}

bool MergeTerrainChanges(TerrainChange *stroke, const TerrainChange &dab) {
  if (!stroke)
    return false;
  if (dab.empty())
    return true;
  if (stroke->empty()) {
    *stroke = dab;
    return true;
  }
  if (stroke->terrain != dab.terrain ||
      !IsSorted(stroke->samples, ChangeLess) ||
      !IsSorted(dab.samples, ChangeLess) ||
      !IsSorted(stroke->dirty_tiles, KeyLess) ||
      !IsSorted(dab.dirty_tiles, KeyLess)) {
    return false;
  }

  TerrainChange merged;
  merged.terrain = stroke->terrain;
  merged.samples.reserve(stroke->samples.size() + dab.samples.size());
  size_t old_index = 0;
  size_t dab_index = 0;
  while (old_index < stroke->samples.size() || dab_index < dab.samples.size()) {
    if (dab_index == dab.samples.size() ||
        (old_index < stroke->samples.size() &&
         ChangeLess(stroke->samples[old_index], dab.samples[dab_index]))) {
      merged.samples.push_back(stroke->samples[old_index++]);
    } else if (old_index == stroke->samples.size() ||
               ChangeLess(dab.samples[dab_index], stroke->samples[old_index])) {
      merged.samples.push_back(dab.samples[dab_index++]);
    } else {
      const TerrainSampleChange &old = stroke->samples[old_index++];
      const TerrainSampleChange &later = dab.samples[dab_index++];
      if (old.new_value != later.old_value)
        return false;
      merged.samples.push_back(
          {old.tile, old.sample, old.old_value, later.new_value});
    }
  }

  merged.dirty_tiles.reserve(stroke->dirty_tiles.size() +
                             dab.dirty_tiles.size());
  // std::set_union: on equal keys the stroke's copy is kept.
  size_t old_tile = 0;
  size_t dab_tile = 0;
  while (old_tile < stroke->dirty_tiles.size()) {
    if (dab_tile == dab.dirty_tiles.size()) {
      merged.dirty_tiles.push_back(stroke->dirty_tiles[old_tile++]);
    } else if (KeyLess(dab.dirty_tiles[dab_tile],
                       stroke->dirty_tiles[old_tile])) {
      merged.dirty_tiles.push_back(dab.dirty_tiles[dab_tile++]);
    } else {
      if (!KeyLess(stroke->dirty_tiles[old_tile], dab.dirty_tiles[dab_tile]))
        ++dab_tile;
      merged.dirty_tiles.push_back(stroke->dirty_tiles[old_tile++]);
    }
  }
  while (dab_tile < dab.dirty_tiles.size())
    merged.dirty_tiles.push_back(dab.dirty_tiles[dab_tile++]);
  *stroke = base::move(merged);
  return true;
}

} // namespace rx::terrain
