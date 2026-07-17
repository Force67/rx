#include "placement/placement.h"

#include <algorithm>
#include <cmath>

#include "placement/density_program.h"
#include "placement/placement_math.h"
#include "placement/placement_pattern.h"

namespace rx::placement {

PlacementSystem::PlacementSystem(const WorldData* world, u32 height_map, PlacementConfig config)
    : world_(world), height_map_(height_map), config_(config) {}

void PlacementSystem::AddEcotope(const Ecotope& ecotope) {
  for (const PlacementLayer& layer : ecotope.layers) layers_.push_back(layer);
}

void PlacementSystem::Compile() {
  stacks_.clear();
  // Stable-partition the flat layer list so same-footprint layers are
  // contiguous in authoring order, then describe each run as a stack.
  base::Vector<PlacementLayer> sorted;
  sorted.reserve(layers_.size());
  base::Vector<bool> taken;
  taken.resize(layers_.size(), false);
  for (std::size_t i = 0; i < layers_.size(); ++i) {
    if (taken[i]) continue;
    PlacementStack stack;
    stack.footprint = layers_[i].footprint;
    stack.tile_size = stack.footprint * static_cast<f32>(kTileFootprints);
    stack.stream_radius =
        stack.tile_size * (static_cast<f32>(config_.radius_tiles) + 0.5f);
    stack.first_layer = static_cast<u32>(sorted.size());
    for (std::size_t j = i; j < layers_.size(); ++j) {
      if (taken[j] || layers_[j].footprint != stack.footprint) continue;
      if (stack.layer_count == kMaxStackLayers) break;
      sorted.push_back(layers_[j]);
      taken[j] = true;
      ++stack.layer_count;
    }
    stacks_.push_back(stack);
  }
  layers_ = std::move(sorted);
}

Vec3 PlacementSystem::TileOrigin(const TileKey& key) const {
  const PlacementStack& stack = stacks_[key.stack];
  return {static_cast<f32>(key.x) * stack.tile_size, 0.0f,
          static_cast<f32>(key.z) * stack.tile_size};
}

PlacementSystem::TileState* PlacementSystem::Find(const TileKey& key) {
  for (TileState& tile : tiles_) {
    if (tile.key == key) return &tile;
  }
  return nullptr;
}

const PlacementSystem::TileState* PlacementSystem::Find(const TileKey& key) const {
  return const_cast<PlacementSystem*>(this)->Find(key);
}

void PlacementSystem::Update(const Vec3& viewer) {
  pending_.clear();
  evicted_.clear();

  // Evict: live or stale tiles whose ring the viewer has left. In-flight
  // tiles are left alone; their results are dropped on completion if the
  // tile is gone by then.
  for (TileState& tile : tiles_) {
    const PlacementStack& stack = stacks_[tile.key.stack];
    f32 cx = (static_cast<f32>(tile.key.x) + 0.5f) * stack.tile_size;
    f32 cz = (static_cast<f32>(tile.key.z) + 0.5f) * stack.tile_size;
    f32 dx = viewer.x - cx;
    f32 dz = viewer.z - cz;
    bool outside = std::sqrt(dx * dx + dz * dz) > stack.stream_radius;
    if (tile.state == 2 && (outside || tile.stale)) evicted_.push_back(tile.key);
  }

  // Want: every tile in the ring around the viewer that is not yet tracked.
  u32 budget = config_.max_jobs_per_update;
  for (u32 s = 0; s < stacks_.size() && budget > 0; ++s) {
    const PlacementStack& stack = stacks_[s];
    i32 r = static_cast<i32>(config_.radius_tiles);
    i32 center_x = static_cast<i32>(std::floor(viewer.x / stack.tile_size));
    i32 center_z = static_cast<i32>(std::floor(viewer.z / stack.tile_size));
    for (i32 dz = -r; dz <= r && budget > 0; ++dz) {
      for (i32 dx = -r; dx <= r && budget > 0; ++dx) {
        TileKey key{s, center_x + dx, center_z + dz};
        f32 cx = (static_cast<f32>(key.x) + 0.5f) * stack.tile_size;
        f32 cz = (static_cast<f32>(key.z) + 0.5f) * stack.tile_size;
        f32 ddx = viewer.x - cx;
        f32 ddz = viewer.z - cz;
        if (std::sqrt(ddx * ddx + ddz * ddz) > stack.stream_radius) continue;
        if (Find(key)) continue;
        pending_.push_back(key);
        --budget;
      }
    }
  }
}

void PlacementSystem::MarkInFlight(const TileKey& key) {
  if (TileState* tile = Find(key)) {
    tile->state = 1;
    tile->stale = false;
    return;
  }
  tiles_.push_back({key, 1, false});
}

void PlacementSystem::MarkLive(const TileKey& key) {
  if (TileState* tile = Find(key)) {
    tile->state = 2;
    return;
  }
  tiles_.push_back({key, 2, false});
}

void PlacementSystem::Release(const TileKey& key) {
  for (std::size_t i = 0; i < tiles_.size(); ++i) {
    if (tiles_[i].key != key) continue;
    tiles_[i] = tiles_[tiles_.size() - 1];
    tiles_.pop_back();
    return;
  }
}

bool PlacementSystem::IsLive(const TileKey& key) const {
  const TileState* tile = Find(key);
  return tile && tile->state == 2;
}

void PlacementSystem::InvalidateRegion(f32 min_x, f32 min_z, f32 max_x, f32 max_z) {
  for (TileState& tile : tiles_) {
    const PlacementStack& stack = stacks_[tile.key.stack];
    // Density texels sample the maps bilinearly and sample positions jitter
    // by a fraction of the footprint; pad by one footprint to cover both.
    f32 pad = stack.footprint;
    f32 tx0 = static_cast<f32>(tile.key.x) * stack.tile_size - pad;
    f32 tz0 = static_cast<f32>(tile.key.z) * stack.tile_size - pad;
    f32 tx1 = tx0 + stack.tile_size + 2.0f * pad;
    f32 tz1 = tz0 + stack.tile_size + 2.0f * pad;
    if (tx1 < min_x || tx0 > max_x || tz1 < min_z || tz0 > max_z) continue;
    tile.stale = true;
  }
}

void PlacementSystem::EmitTileCpu(const TileKey& key, base::Vector<PlacedInstance>& out) const {
  const PlacementStack& stack = stacks_[key.stack];
  const Vec3 origin = TileOrigin(key);
  const f32 texel_size = stack.tile_size / static_cast<f32>(kDensityResolution);

  // DENSITYMAP: cumulative density per layer per texel (layered dithering -
  // layer k owns the threshold interval [cum[k-1], cum[k]) at each texel).
  base::Vector<f32> density;
  density.resize(static_cast<std::size_t>(kDensityResolution) * kDensityResolution *
                     stack.layer_count,
                 0.0f);
  for (u32 tz = 0; tz < kDensityResolution; ++tz) {
    for (u32 tx = 0; tx < kDensityResolution; ++tx) {
      f32 wx = origin.x + (static_cast<f32>(tx) + 0.5f) * texel_size;
      f32 wz = origin.z + (static_cast<f32>(tz) + 0.5f) * texel_size;
      f32 cumulative = 0.0f;
      for (u32 layer = 0; layer < stack.layer_count; ++layer) {
        const PlacementLayer& desc = layers_[stack.first_layer + layer];
        cumulative += EvalDensityProgram(desc.density.ops(), *world_, wx, wz);
        cumulative = std::min(cumulative, 1.0f);
        density[(static_cast<std::size_t>(layer) * kDensityResolution + tz) *
                    kDensityResolution +
                tx] = cumulative;
      }
    }
  }

  // GENERATE + PLACEMENT: threshold-test every pattern point, snap survivors
  // to the height field and build their transforms.
  for (u32 i = 0; i < kPatternPointCount; ++i) {
    u32 jitter_seed =
        InstanceSeed(config_.seed ^ 0x51AB71EDu, key.x, key.z, i, key.stack);
    f32 jitter_x = (HashToUnit(jitter_seed) - 0.5f) * config_.jitter * stack.footprint;
    f32 jitter_z =
        (HashToUnit(PcgHash(jitter_seed)) - 0.5f) * config_.jitter * stack.footprint;
    f32 local_x = kPatternXY[i * 2 + 0] * stack.tile_size + jitter_x;
    f32 local_z = kPatternXY[i * 2 + 1] * stack.tile_size + jitter_z;
    if (local_x < 0.0f || local_x >= stack.tile_size || local_z < 0.0f ||
        local_z >= stack.tile_size) {
      continue;  // jittered off the tile; the neighbour does not own it either
    }

    f32 threshold = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kPatternPointCount);
    u32 texel_x = std::min(static_cast<u32>(local_x / texel_size), kDensityResolution - 1);
    u32 texel_z = std::min(static_cast<u32>(local_z / texel_size), kDensityResolution - 1);

    u32 selected = kMaxStackLayers;
    f32 below = 0.0f;
    for (u32 layer = 0; layer < stack.layer_count; ++layer) {
      f32 cumulative = density[(static_cast<std::size_t>(layer) * kDensityResolution +
                                texel_z) *
                                   kDensityResolution +
                               texel_x];
      if (threshold < cumulative && threshold >= below) {
        selected = layer;
        break;
      }
      below = cumulative;
    }
    if (selected == kMaxStackLayers) continue;

    const u32 layer_index = stack.first_layer + selected;
    const PlacementLayer& desc = layers_[layer_index];

    f32 world_x = origin.x + local_x;
    f32 world_z = origin.z + local_z;
    OrientedPoint point;
    point.position = {world_x, world_->Sample(height_map_, world_x, world_z), world_z};
    // Central differences over the height map, one map texel apart - the
    // data is already in cache from the height sample.
    f32 h = world_->meters_per_texel();
    f32 dhx = world_->Sample(height_map_, world_x + h, world_z) -
              world_->Sample(height_map_, world_x - h, world_z);
    f32 dhz = world_->Sample(height_map_, world_x, world_z + h) -
              world_->Sample(height_map_, world_x, world_z - h);
    Vec3 normal{-dhx, 2.0f * h, -dhz};
    f32 nlen = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    point.normal = {normal.x / nlen, normal.y / nlen, normal.z / nlen};

    u32 seed = InstanceSeed(config_.seed, key.x, key.z, i, layer_index);
    f32 yaw = desc.random_yaw ? HashToUnit(seed) * 6.28318530f : 0.0f;
    f32 scale =
        desc.scale_min + (desc.scale_max - desc.scale_min) * HashToUnit(PcgHash(seed));

    PlacedInstance instance;
    instance.transform =
        BuildPlacementTransform(point, yaw, scale, desc.tilt, desc.y_offset);
    instance.layer = layer_index;
    instance.point = i;
    instance.tile_x = key.x;
    instance.tile_z = key.z;
    out.push_back(instance);
  }
}

}  // namespace rx::placement
