#include "demo_placement.h"

#include <cmath>
#include <cstdlib>

#include "asset/primitives.h"
#include "core/log.h"
#include "placement/placement_math.h"
#include "placement_demo_assets.h"
#include "scene/components.h"

namespace rx {
namespace {

// The world: a 1024 m square centred on the origin, described at 2 m/texel.
constexpr f32 kWorldExtent = 1024.0f;
constexpr f32 kWorldOrigin = -512.0f;
constexpr u32 kWorldResolution = 512;

// Terrain mesh density (vertices span the whole WorldData region).
constexpr u32 kTerrainGrid = 384;

// The road winds north-south through the world.
f32 RoadCenter(f32 z) { return 90.0f * std::sin(z * 0.004f) + 30.0f * std::sin(z * 0.011f); }
constexpr f32 kRoadHalfWidth = 4.5f;

// The lake basin carved into the height field.
constexpr f32 kLakeX = 170.0f;
constexpr f32 kLakeZ = -140.0f;
constexpr f32 kLakeRadius = 110.0f;
constexpr f32 kWaterLevel = -3.2f;

u32 PackColor(f32 r, f32 g, f32 b) {
  auto to8 = [](f32 v) { return static_cast<u32>((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f); };
  return 0xff000000u | (to8(b) << 16) | (to8(g) << 8) | to8(r);
}

f32 Fbm(f32 x, f32 z, f32 base_size, u32 seed, u32 octaves) {
  f32 sum = 0.0f;
  f32 amp = 0.5f;
  f32 size = base_size;
  for (u32 i = 0; i < octaves; ++i) {
    sum += (placement::ValueNoise(x, z, size, seed + i) - 0.5f) * 2.0f * amp;
    amp *= 0.5f;
    size *= 0.5f;
  }
  return sum;
}

}  // namespace

PlacementDemo::PlacementDemo(EngineContext& ctx)
    : ctx_(ctx), world_(kWorldOrigin, kWorldOrigin, kWorldExtent, kWorldResolution) {
  const char* lines = std::getenv("RX_PLACEMENT_LINES");
  draw_lines_ = lines && lines[0] == '1';
  const char* fly = std::getenv("RX_PLACEMENT_FLY");
  autopilot_ = fly && fly[0] == '1';
}

PlacementDemo::~PlacementDemo() = default;

f32 PlacementDemo::TerrainHeight(f32 x, f32 z) const {
  // Rolling hills with a broad ridge, softened toward the road and carved
  // into a basin around the lake.
  f32 height = 10.0f * Fbm(x, z, 420.0f, 101u, 4) + 3.0f * Fbm(x, z, 90.0f, 202u, 3);
  f32 road_dist = std::fabs(x - RoadCenter(z));
  f32 road_blend = std::min(road_dist / 22.0f, 1.0f);
  road_blend = road_blend * road_blend * (3.0f - 2.0f * road_blend);
  f32 dx = x - kLakeX;
  f32 dz = z - kLakeZ;
  f32 lake_dist = std::sqrt(dx * dx + dz * dz);
  f32 basin = std::min(lake_dist / kLakeRadius, 1.0f);
  basin = basin * basin * (3.0f - 2.0f * basin);
  height = height * (0.35f + 0.65f * road_blend);  // road hugs gentler ground
  height = height * basin + (kWaterLevel - 4.5f) * (1.0f - basin);
  return height;
}

void PlacementDemo::BuildWorldData() {
  map_height_ = world_.AddMap("height");
  map_forest_ = world_.AddMap("forest");
  map_road_ = world_.AddMap("road");
  map_water_ = world_.AddMap("water");

  world_.Generate(map_height_, [this](f32 x, f32 z) { return TerrainHeight(x, z); });
  // Forest coverage: low-frequency noise shaped into a 0..1 mask whose value
  // encodes meaning - sparse edge below ~0.5, dense interior above (the
  // density programs decode the bands differently per tree species).
  world_.Generate(map_forest_, [](f32 x, f32 z) {
    f32 base = placement::ValueNoise(x, z, 340.0f, 11u);
    f32 detail = placement::ValueNoise(x, z, 130.0f, 12u);
    f32 v = base * 0.7f + detail * 0.3f;
    f32 t = std::clamp((v - 0.38f) / 0.34f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
  });
  world_.Generate(map_road_, [](f32 x, f32 z) {
    f32 d = std::fabs(x - RoadCenter(z));
    return std::clamp(1.0f - (d - kRoadHalfWidth) / 4.0f, 0.0f, 1.0f);
  });
  world_.Generate(map_water_, [this](f32 x, f32 z) {
    return TerrainHeight(x, z) < kWaterLevel + 0.4f ? 1.0f : 0.0f;
  });

  // A painted clearing near the spawn: artist data layered over generated
  // maps, and the demo's local-stability showpiece.
  world_.PaintDisc(map_forest_, 40.0f, 55.0f, 26.0f, 0.0f);
}

void PlacementDemo::BuildEcotopes() {
  using placement::DensityProgram;
  using placement::Ecotope;
  using placement::PlacementLayer;

  placement::PlacementConfig config;
  config.radius_tiles = 3;
  system_ = std::make_unique<placement::PlacementSystem>(&world_, map_height_, config);

  auto exclude = [this](DensityProgram& p) {
    p.Map(map_road_).OneMinus().Mul().Map(map_water_).OneMinus().Mul();
  };

  Ecotope forest;
  forest.name = "forest";
  {
    // Interior species: tall pines dominate where the forest mask is dense.
    PlacementLayer pine;
    pine.name = "pine";
    pine.footprint = 6.0f;
    pine.density.Map(map_forest_).Range(0.45f, 0.85f).Const(0.62f).Mul();
    exclude(pine.density);
    pine.scale_min = 0.75f;
    pine.scale_max = 1.35f;
    pine.tilt = 0.12f;
    forest.layers.push_back(pine);
  }
  {
    // Edge species: lush broadleaves take the band where the mask ramps up,
    // fading out again inside the dense interior.
    PlacementLayer broadleaf;
    broadleaf.name = "broadleaf";
    broadleaf.footprint = 6.0f;
    broadleaf.density.Map(map_forest_)
        .Range(0.12f, 0.5f)
        .Map(map_forest_)
        .Range(0.6f, 0.95f)
        .OneMinus()
        .Mul()
        .Const(0.5f)
        .Mul();
    exclude(broadleaf.density);
    broadleaf.scale_min = 0.7f;
    broadleaf.scale_max = 1.2f;
    forest.layers.push_back(broadleaf);
  }
  {
    // Rare snags scattered through the interior.
    PlacementLayer dead;
    dead.name = "dead_tree";
    dead.footprint = 6.0f;
    dead.density.Map(map_forest_)
        .Range(0.5f, 1.0f)
        .Noise(48.0f, 31.0f)
        .Pow(3.0f)
        .Mul()
        .Const(0.22f)
        .Mul();
    exclude(dead.density);
    dead.scale_min = 0.8f;
    dead.scale_max = 1.1f;
    dead.tilt = 0.3f;
    forest.layers.push_back(dead);
  }
  {
    PlacementLayer bush;
    bush.name = "bush";
    bush.footprint = 2.5f;
    bush.density.Map(map_forest_)
        .Range(0.1f, 0.45f)
        .Noise(36.0f, 41.0f)
        .Mul()
        .Const(0.55f)
        .Mul();
    exclude(bush.density);
    bush.scale_min = 0.7f;
    bush.scale_max = 1.4f;
    forest.layers.push_back(bush);
  }
  {
    PlacementLayer rock;
    rock.name = "rock";
    rock.footprint = 2.5f;
    // Rocky patches, thinned to half under dense forest, never in the lake.
    rock.density.Noise(70.0f, 51.0f)
        .Range(0.58f, 0.8f)
        .Map(map_forest_)
        .Const(0.5f)
        .Mul()
        .OneMinus()
        .Mul()
        .Const(0.5f)
        .Mul();
    rock.density.Map(map_water_).OneMinus().Mul();
    rock.scale_min = 0.5f;
    rock.scale_max = 1.8f;
    rock.tilt = 0.55f;
    rock.random_yaw = true;
    forest.layers.push_back(rock);
  }
  {
    PlacementLayer grass;
    grass.name = "grass";
    grass.footprint = 1.2f;
    grass.density.Noise(28.0f, 61.0f).Const(0.55f).Mul().Const(0.3f).Add();
    exclude(grass.density);
    grass.scale_min = 0.6f;
    grass.scale_max = 1.3f;
    grass.tilt = 0.4f;
    forest.layers.push_back(grass);
  }
  system_->AddEcotope(forest);
  system_->Compile();

  layer_stacks_.clear();
  layer_stacks_.resize(system_->layers().size(), 0u);
  for (u32 s = 0; s < system_->stacks().size(); ++s) {
    const placement::PlacementStack& stack = system_->stacks()[s];
    for (u32 l = stack.first_layer; l < stack.first_layer + stack.layer_count; ++l) {
      layer_stacks_[l] = s;
    }
  }
}

void PlacementDemo::BuildTerrainMesh() {
  asset::Material ground;
  ground.id = asset::MakeAssetId("placement/ground");
  ground.base_color_factor[0] = 1.0f;
  ground.base_color_factor[1] = 1.0f;
  ground.base_color_factor[2] = 1.0f;
  ground.roughness_factor = 1.0f;
  ground.metallic_factor = 0.0f;

  asset::Mesh terrain;
  terrain.id = asset::MakeAssetId("placement/terrain");
  asset::MeshLod& lod = terrain.lods.emplace_back();
  const f32 step = kWorldExtent / static_cast<f32>(kTerrainGrid);
  for (u32 gz = 0; gz <= kTerrainGrid; ++gz) {
    for (u32 gx = 0; gx <= kTerrainGrid; ++gx) {
      const f32 x = kWorldOrigin + static_cast<f32>(gx) * step;
      const f32 z = kWorldOrigin + static_cast<f32>(gz) * step;
      asset::Vertex v = {};
      v.position[0] = x;
      // The mesh samples the same map the placement pipeline snaps to, so
      // instances sit on the rendered surface.
      v.position[1] = world_.Sample(map_height_, x, z);
      v.position[2] = z;
      const f32 h = world_.meters_per_texel();
      const f32 dhx =
          world_.Sample(map_height_, x + h, z) - world_.Sample(map_height_, x - h, z);
      const f32 dhz =
          world_.Sample(map_height_, x, z + h) - world_.Sample(map_height_, x, z - h);
      const f32 inv =
          1.0f / std::sqrt(dhx * dhx + dhz * dhz + 4.0f * h * h);
      v.normal[0] = -dhx * inv;
      v.normal[1] = 2.0f * h * inv;
      v.normal[2] = -dhz * inv;
      v.uv[0] = x * 0.25f;
      v.uv[1] = z * 0.25f;

      const f32 forest = world_.Sample(map_forest_, x, z);
      const f32 road = world_.Sample(map_road_, x, z);
      const f32 water = world_.Sample(map_water_, x, z);
      f32 r = 0.26f + 0.1f * (1.0f - forest);
      f32 g = 0.4f - 0.12f * forest;
      f32 b = 0.16f;
      r = r + (0.48f - r) * road;  // packed dirt
      g = g + (0.42f - g) * road;
      b = b + (0.3f - b) * road;
      r = r + (0.2f - r) * water;  // wet silt under the lake
      g = g + (0.24f - g) * water;
      b = b + (0.2f - b) * water;
      v.color = PackColor(r, g, b);
      lod.vertices.push_back(v);
    }
  }
  for (u32 gz = 0; gz < kTerrainGrid; ++gz) {
    for (u32 gx = 0; gx < kTerrainGrid; ++gx) {
      const u32 a = gz * (kTerrainGrid + 1) + gx;
      const u32 b = a + 1;
      const u32 c = a + (kTerrainGrid + 1);
      const u32 d = c + 1;
      for (u32 index : {a, c, b, b, c, d}) lod.indices.push_back(index);
    }
  }
  asset::Submesh submesh;
  submesh.index_count = static_cast<u32>(lod.indices.size());
  submesh.material = ground.id;
  lod.submeshes.push_back(submesh);
  terrain.bounds_radius = kWorldExtent;

  // A flat sheet over the lake basin.
  asset::Material water_material;
  water_material.id = asset::MakeAssetId("placement/water_mat");
  water_material.base_color_factor[0] = 0.09f;
  water_material.base_color_factor[1] = 0.22f;
  water_material.base_color_factor[2] = 0.3f;
  water_material.roughness_factor = 0.08f;
  water_material.metallic_factor = 0.0f;
  asset::Mesh water = asset::MakeBox(kLakeRadius * 1.6f, 0.05f, kLakeRadius * 1.6f,
                                     asset::MakeAssetId("placement/water"));
  for (auto& sm : water.lods[0].submeshes) sm.material = water_material.id;
  if (water.lods[0].submeshes.empty()) {
    water.lods[0].submeshes.push_back(
        {0, static_cast<u32>(water.lods[0].indices.size()), water_material.id});
  }

  if (!ctx_.config->headless) {
    ctx_.renderer->UploadMaterial(ground);
    ctx_.renderer->UploadMesh(terrain);
    ctx_.renderer->UploadMaterial(water_material);
    ctx_.renderer->UploadMesh(water);
  }
  ecs::Entity ground_entity = ctx_.world->Create();
  ctx_.world->Add(ground_entity, scene::Transform{});
  ctx_.world->Add(ground_entity, scene::Renderable{terrain.id});
  ecs::Entity water_entity = ctx_.world->Create();
  scene::Transform water_transform;
  water_transform.position[0] = kLakeX;
  water_transform.position[1] = kWaterLevel;
  water_transform.position[2] = kLakeZ;
  ctx_.world->Add(water_entity, water_transform);
  ctx_.world->Add(water_entity, scene::Renderable{water.id});
}

void PlacementDemo::Create() {
  BuildWorldData();
  BuildEcotopes();
  BuildTerrainMesh();

  // The hand-authored asset set the ecotope references, one mesh per layer:
  // pine, broadleaf, dead tree, bush, rock, grass (compiled layer order).
  asset::Material trunk;
  trunk.id = asset::MakeAssetId("placement/trunk");
  trunk.base_color_factor[0] = 0.32f;
  trunk.base_color_factor[1] = 0.22f;
  trunk.base_color_factor[2] = 0.13f;
  asset::Material pine_canopy;
  pine_canopy.id = asset::MakeAssetId("placement/pine_canopy");
  pine_canopy.base_color_factor[0] = 0.1f;
  pine_canopy.base_color_factor[1] = 0.3f;
  pine_canopy.base_color_factor[2] = 0.14f;
  asset::Material leaf_canopy;
  leaf_canopy.id = asset::MakeAssetId("placement/leaf_canopy");
  leaf_canopy.base_color_factor[0] = 0.2f;
  leaf_canopy.base_color_factor[1] = 0.42f;
  leaf_canopy.base_color_factor[2] = 0.12f;
  asset::Material dead_wood;
  dead_wood.id = asset::MakeAssetId("placement/dead_wood");
  dead_wood.base_color_factor[0] = 0.35f;
  dead_wood.base_color_factor[1] = 0.3f;
  dead_wood.base_color_factor[2] = 0.24f;
  asset::Material bush_leaf;
  bush_leaf.id = asset::MakeAssetId("placement/bush_leaf");
  bush_leaf.base_color_factor[0] = 0.16f;
  bush_leaf.base_color_factor[1] = 0.36f;
  bush_leaf.base_color_factor[2] = 0.13f;
  asset::Material stone;
  stone.id = asset::MakeAssetId("placement/stone");
  stone.base_color_factor[0] = 0.34f;
  stone.base_color_factor[1] = 0.33f;
  stone.base_color_factor[2] = 0.31f;
  asset::Material grass_blade;
  grass_blade.id = asset::MakeAssetId("placement/grass_blade");
  grass_blade.base_color_factor[0] = 0.24f;
  grass_blade.base_color_factor[1] = 0.44f;
  grass_blade.base_color_factor[2] = 0.12f;
  grass_blade.two_sided = true;

  asset::Mesh meshes[] = {
      MakePineTree(9.0f, trunk.id.hash, pine_canopy.id.hash,
                   asset::MakeAssetId("placement/pine")),
      MakeBroadleafTree(7.0f, trunk.id.hash, leaf_canopy.id.hash,
                        asset::MakeAssetId("placement/broadleaf")),
      MakeDeadTree(6.0f, dead_wood.id.hash, asset::MakeAssetId("placement/dead_tree")),
      MakeBush(1.1f, bush_leaf.id.hash, asset::MakeAssetId("placement/bush")),
      MakeRock(1.0f, 7u, stone.id.hash, asset::MakeAssetId("placement/rock")),
      MakeGrassTuft(0.55f, grass_blade.id.hash, asset::MakeAssetId("placement/grass")),
  };
  if (!ctx_.config->headless) {
    for (const asset::Material& material :
         {trunk, pine_canopy, leaf_canopy, dead_wood, bush_leaf, stone, grass_blade}) {
      ctx_.renderer->UploadMaterial(material);
    }
    for (const asset::Mesh& mesh : meshes) ctx_.renderer->UploadMesh(mesh);
  }

  // Compiled layer order == authoring order per stack; map layer -> mesh.
  layer_meshes_.clear();
  for (const placement::PlacementLayer& layer : system_->layers()) {
    u64 mesh = 0;
    if (layer.name == "pine") mesh = meshes[0].id.hash;
    if (layer.name == "broadleaf") mesh = meshes[1].id.hash;
    if (layer.name == "dead_tree") mesh = meshes[2].id.hash;
    if (layer.name == "bush") mesh = meshes[3].id.hash;
    if (layer.name == "rock") mesh = meshes[4].id.hash;
    if (layer.name == "grass") mesh = meshes[5].id.hash;
    layer_meshes_.push_back(mesh);
  }

  const Vec3 spawn{40.0f, 0.0f, 40.0f};
  ctx_.camera->set_position({spawn.x, world_.Sample(map_height_, spawn.x, spawn.z) + 4.0f,
                             spawn.z});
  ctx_.camera->set_yaw_pitch(-0.7f, -0.15f);
  ctx_.camera->speed = 12.0f;

  if (ctx_.config->headless || !ctx_.renderer || !ctx_.renderer->device()) return;
  if (!gpu_.Initialize(*ctx_.renderer->device(), *system_)) {
    RX_WARN("placement demo: GPU pipeline unavailable; world stays empty");
    return;
  }
  gpu_.SyncWorldData(*ctx_.renderer->device(), world_);
  gpu_ready_ = true;

  // Fill the whole start ring synchronously so the first frame is a dressed
  // world; from here on tiles stream through the per-frame batches.
  base::Vector<placement::PlacedInstance> initial;
  for (u32 rounds = 0; rounds < 64; ++rounds) {
    system_->Update(ctx_.camera->position());
    if (system_->pending().empty()) break;
    gpu_.GenerateImmediate(*ctx_.renderer->device(), *system_, system_->pending(), initial);
  }
  ApplyResults({initial.data(), initial.size()});
  RX_INFO("placement demo: {} instances across {} tiles at spawn", initial.size(),
          live_.size());
}

void PlacementDemo::ApplyResults(std::span<const placement::PlacedInstance> instances) {
  if (instances.empty()) return;
  // Bucket by (layer, tile) and build one instance group per bucket; groups
  // attach to their tile so eviction can retire them.
  struct Bucket {
    u32 layer;
    i32 tile_x;
    i32 tile_z;
    base::Vector<Mat4> transforms;
  };
  base::Vector<Bucket> buckets;
  for (const placement::PlacedInstance& instance : instances) {
    Bucket* bucket = nullptr;
    for (Bucket& candidate : buckets) {
      if (candidate.layer == instance.layer && candidate.tile_x == instance.tile_x &&
          candidate.tile_z == instance.tile_z) {
        bucket = &candidate;
        break;
      }
    }
    if (!bucket) {
      buckets.push_back({instance.layer, instance.tile_x, instance.tile_z, {}});
      bucket = &buckets[buckets.size() - 1];
    }
    bucket->transforms.push_back(instance.transform);
  }

  for (Bucket& bucket : buckets) {
    if (bucket.layer >= layer_meshes_.size() || layer_meshes_[bucket.layer] == 0) continue;
    placement::TileKey key{layer_stacks_[bucket.layer], bucket.tile_x, bucket.tile_z};
    render::InstanceGroupHandle group = ctx_.renderer->CreateInstanceGroup(
        layer_meshes_[bucket.layer],
        {bucket.transforms.data(), bucket.transforms.size()});
    if (!group) {
      RX_WARN("placement demo: instance group rejected (layer {}, {} instances)",
              bucket.layer, bucket.transforms.size());
      continue;
    }
    TileGroups* tile = nullptr;
    for (TileGroups& candidate : live_) {
      if (candidate.key == key) {
        tile = &candidate;
        break;
      }
    }
    if (!tile) {
      live_.push_back({key, {}});
      tile = &live_[live_.size() - 1];
    }
    tile->groups.push_back(group);
  }
}

void PlacementDemo::DestroyTileGroups(const placement::TileKey& key) {
  for (u32 i = 0; i < live_.size(); ++i) {
    if (!(live_[i].key == key)) continue;
    for (render::InstanceGroupHandle group : live_[i].groups) {
      ctx_.renderer->DestroyInstanceGroup(group);
    }
    live_[i] = live_[live_.size() - 1];
    live_.pop_back();
    return;
  }
}

void PlacementDemo::Emit(f32 dt, render::FrameView& view) {
  if (!gpu_ready_) return;

  // Autopilot (RX_PLACEMENT_FLY=1): drift along the road so streaming can be
  // watched and captured without input.
  if (autopilot_) {
    fly_time_ += dt;
    const f32 z = 40.0f + fly_time_ * 14.0f;
    const f32 x = RoadCenter(z) + 10.0f;
    ctx_.camera->set_position({x, world_.Sample(map_height_, x, z) + 5.0f, z});
    ctx_.camera->set_yaw_pitch(-3.1f, -0.12f);
  }

  // Instance groups harvested by last frame's hook become renderer state
  // here, outside command recording.
  if (!harvested_tiles_.empty() || !harvested_.empty()) {
    ApplyResults({harvested_.data(), harvested_.size()});
    harvested_.clear();
    harvested_tiles_.clear();
  }

  system_->Update(ctx_.camera->position());
  for (const placement::TileKey& key : system_->evicted()) {
    DestroyTileGroups(key);
    system_->Release(key);
  }

  view.scene_opaque = [this](const render::SceneHookContext& ctx) {
    // The slot's fence passed at frame begin: whatever this slot recorded
    // frames-in-flight ago is finished. Harvest, then refill the slot.
    gpu_.Consume(ctx.frame_slot, *system_, harvested_, harvested_tiles_);
    gpu_.RecordJobs(*ctx.cmd, *system_, ctx.frame_slot);
  };

  if (draw_lines_) {
    lines_.clear();
    for (const TileGroups& tile : live_) {
      const placement::PlacementStack& stack = system_->stacks()[tile.key.stack];
      const Vec3 origin = system_->TileOrigin(tile.key);
      const f32 y = ctx_.camera->position().y - 2.0f;
      const u32 color = 0xff40c080u >> (tile.key.stack * 2u);
      const f32 size = stack.tile_size;
      lines_.push_back({{origin.x, y, origin.z}, {origin.x + size, y, origin.z}, color});
      lines_.push_back({{origin.x + size, y, origin.z},
                        {origin.x + size, y, origin.z + size}, color});
      lines_.push_back({{origin.x + size, y, origin.z + size},
                        {origin.x, y, origin.z + size}, color});
      lines_.push_back({{origin.x, y, origin.z + size}, {origin.x, y, origin.z}, color});
    }
    view.debug_lines_overlay = {lines_.data(), lines_.size()};
  }
}

void PlacementDemo::Shutdown() {
  if (!gpu_ready_) return;
  for (const TileGroups& tile : live_) {
    for (render::InstanceGroupHandle group : tile.groups) {
      ctx_.renderer->DestroyInstanceGroup(group);
    }
  }
  live_.clear();
  gpu_.Shutdown(*ctx_.renderer->device());
  gpu_ready_ = false;
}

}  // namespace rx
