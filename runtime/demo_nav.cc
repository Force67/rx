#include "demo_nav.h"

#include <cmath>
#include <cstdlib>

#include "asset/primitives.h"
#include "core/log.h"
#include "nav/nav_debug.h"
#include "nav/path.h"
#include "scene/components.h"

namespace rx {
namespace {

// Areas beyond plain ground. Rock and water stay walkable -- they just cost:
// per-meter multipliers make pathfinding prefer smooth routes, entry tolls
// make agents COMMIT once they stepped in instead of bouncing back out.
constexpr nav::AreaId kAreaRock = 2;
constexpr nav::AreaId kAreaWater = 3;

constexpr f32 kWorldSize = 96.0f;  // terrain spans [0, 96]^2 meters

// The boulder fields: painted into the navmesh as expensive area and dressed
// with visible rocks. Deterministic layout.
struct RockField {
  f32 x, z, radius;
};
constexpr RockField kRockFields[] = {
    {22, 22, 6.5f}, {45, 14, 5.0f}, {64, 24, 7.0f}, {14, 44, 5.5f},
    {84, 60, 6.0f}, {36, 78, 6.5f}, {58, 68, 5.0f}, {80, 82, 7.5f},
};

// Two mesas with slopes too steep to climb: pathfinding has to route around.
// Placed clear of the river so the carved bed and the water sheet never
// intersect their flanks.
struct Mesa {
  f32 x, z, radius, height;
};
constexpr Mesa kMesas[] = {{18, 74, 7.0f, 5.0f}, {76, 14, 8.0f, 6.0f}};

f32 RiverCenter(f32 x) { return 48.0f + 7.0f * std::sin(x * 0.07f); }
constexpr f32 kRiverHalfWidth = 3.2f;

f32 Smoothstep(f32 t) {
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  return t * t * (3 - 2 * t);
}

u32 NextRand(u32& state) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

f32 RandRange(u32& state, f32 lo, f32 hi) {
  return lo + (hi - lo) * (static_cast<f32>(NextRand(state) & 0xffff) / 65535.0f);
}

u32 PackColor(f32 r, f32 g, f32 b) {
  auto to8 = [](f32 v) { return static_cast<u32>((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f); };
  return 0xff000000u | (to8(b) << 16) | (to8(g) << 8) | to8(r);
}

}  // namespace

NavDemo::NavDemo(EngineContext& ctx)
    : ctx_(ctx), mesh_(nav::NavMeshConfig{.cell_size = 0.5f, .tile_cells = 16}) {}

NavDemo::Ground NavDemo::SampleGround(f32 x, f32 z) const {
  Ground g;
  // Rolling hills, three octaves.
  g.height = 2.2f * std::sin(x * 0.06f) * std::cos(z * 0.05f) +
             1.1f * std::sin(x * 0.13f + 1.7f) * std::sin(z * 0.11f + 0.6f) +
             0.4f * std::sin(x * 0.31f) * std::cos(z * 0.29f);

  // Mesas: flat tops, steep unwalkable flanks (the slope check below).
  for (const Mesa& mesa : kMesas) {
    const f32 dx = x - mesa.x;
    const f32 dz = z - mesa.z;
    const f32 d = std::sqrt(dx * dx + dz * dz);
    g.height += mesa.height * Smoothstep((mesa.radius - d) / 3.0f);
  }

  // The river carves a bed below the banks.
  const f32 river_d = std::fabs(z - RiverCenter(x));
  if (river_d < kRiverHalfWidth + 1.5f) {
    const f32 t = Smoothstep((kRiverHalfWidth + 1.5f - river_d) / (kRiverHalfWidth + 1.5f));
    g.height -= 2.0f * t;
    if (river_d < kRiverHalfWidth) g.area = kAreaWater;
  }

  // Boulder fields on dry ground.
  if (g.area == nav::kAreaGround) {
    for (const RockField& field : kRockFields) {
      const f32 dx = x - field.x;
      const f32 dz = z - field.z;
      if (dx * dx + dz * dz < field.radius * field.radius) {
        g.area = kAreaRock;
        break;
      }
    }
  }
  return g;
}

void NavDemo::BuildTerrainMesh() {
  // One vertex per navmesh cell corner so the render surface and the nav
  // surface agree. Vertex color encodes desirability: grass, grey rock, blue
  // river bed, dark scree on unwalkable slopes.
  asset::Material terrain_material;
  terrain_material.id = asset::MakeAssetId("nav/terrain_material");
  terrain_material.roughness_factor = 0.95f;
  terrain_material.metallic_factor = 0;

  asset::Mesh terrain;
  terrain.id = asset::MakeAssetId("nav/terrain");
  terrain.lods.emplace_back();
  asset::MeshLod& lod = terrain.lods[0];
  constexpr u32 kGrid = 192;  // 0.5 m spacing over 96 m
  constexpr f32 kStep = kWorldSize / kGrid;
  for (u32 gz = 0; gz <= kGrid; ++gz) {
    for (u32 gx = 0; gx <= kGrid; ++gx) {
      const f32 x = gx * kStep;
      const f32 z = gz * kStep;
      const Ground g = SampleGround(x, z);
      asset::Vertex v{};
      v.position[0] = x;
      v.position[1] = g.height;
      v.position[2] = z;
      const f32 hx = SampleGround(x + 0.4f, z).height - SampleGround(x - 0.4f, z).height;
      const f32 hz = SampleGround(x, z + 0.4f).height - SampleGround(x, z - 0.4f).height;
      const Vec3 n = Normalize(Vec3{-hx, 0.8f, -hz});
      v.normal[0] = n.x;
      v.normal[1] = n.y;
      v.normal[2] = n.z;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.uv[0] = x / 8.0f;
      v.uv[1] = z / 8.0f;
      const f32 slope = std::sqrt(hx * hx + hz * hz) / 0.8f;
      u32 tone = PackColor(0.13f + 0.04f * std::sin(x * 0.9f) * std::sin(z * 0.77f), 0.34f,
                           0.10f);  // grass, gently mottled
      if (g.area == kAreaRock) tone = PackColor(0.32f, 0.30f, 0.27f);
      if (g.area == kAreaWater) tone = PackColor(0.10f, 0.16f, 0.30f);
      if (slope > 1.1f) tone = PackColor(0.20f, 0.17f, 0.14f);  // scree flank
      v.color = tone;
      lod.vertices.push_back(v);
    }
  }
  for (u32 gz = 0; gz < kGrid; ++gz) {
    for (u32 gx = 0; gx < kGrid; ++gx) {
      const u32 a = gz * (kGrid + 1) + gx;
      const u32 b = a + 1;
      const u32 c = a + (kGrid + 1);
      const u32 d = c + 1;
      for (u32 index : {a, b, c, b, d, c}) lod.indices.push_back(index);
    }
  }
  asset::Submesh submesh;
  submesh.index_count = static_cast<u32>(lod.indices.size());
  submesh.material = terrain_material.id;
  lod.submeshes.push_back(submesh);
  terrain.bounds_center[0] = kWorldSize * 0.5f;
  terrain.bounds_center[2] = kWorldSize * 0.5f;
  terrain.bounds_radius = kWorldSize;

  // The river surface: a translucent ribbon following the centerline, resting
  // a little above the carved bed.
  asset::Material water_material;
  water_material.id = asset::MakeAssetId("nav/water_material");
  water_material.base_color_factor[0] = 0.10f;
  water_material.base_color_factor[1] = 0.22f;
  water_material.base_color_factor[2] = 0.38f;
  water_material.base_color_factor[3] = 0.62f;
  water_material.roughness_factor = 0.08f;
  water_material.metallic_factor = 0;
  water_material.alpha_mode = asset::AlphaMode::kBlend;
  water_material.two_sided = true;

  asset::Mesh water;
  water.id = asset::MakeAssetId("nav/water");
  water.lods.emplace_back();
  asset::MeshLod& wlod = water.lods[0];
  constexpr u32 kRibbon = 96;
  for (u32 i = 0; i <= kRibbon; ++i) {
    const f32 x = kWorldSize * static_cast<f32>(i) / kRibbon;
    const f32 zc = RiverCenter(x);
    // Water level: bank height minus a hand span, so the sheet sits in the bed.
    const f32 level = SampleGround(x, zc).height + 0.9f;
    for (const f32 side : {-1.0f, 1.0f}) {
      asset::Vertex v{};
      v.position[0] = x;
      v.position[1] = level;
      v.position[2] = zc + side * (kRiverHalfWidth + 0.4f);
      v.normal[1] = 1;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.uv[0] = x / 6.0f;
      v.uv[1] = side;
      v.color = 0xffffffff;
      wlod.vertices.push_back(v);
    }
  }
  for (u32 i = 0; i < kRibbon; ++i) {
    const u32 a = i * 2, b = a + 1, c = a + 2, d = a + 3;
    for (u32 index : {a, b, c, b, d, c}) wlod.indices.push_back(index);
  }
  asset::Submesh wsub;
  wsub.index_count = static_cast<u32>(wlod.indices.size());
  wsub.material = water_material.id;
  wlod.submeshes.push_back(wsub);
  water.bounds_center[0] = kWorldSize * 0.5f;
  water.bounds_center[2] = kWorldSize * 0.5f;
  water.bounds_radius = kWorldSize;

  if (!ctx_.config->headless) {
    ctx_.renderer->UploadMaterial(terrain_material);
    ctx_.renderer->UploadMesh(terrain);
    ctx_.renderer->UploadMaterial(water_material);
    ctx_.renderer->UploadMesh(water);
  }
  ecs::Entity ground = ctx_.world->Create();
  ctx_.world->Add(ground, scene::Transform{});
  ctx_.world->Add(ground, scene::Renderable{terrain.id});
  ecs::Entity sheet = ctx_.world->Create();
  ctx_.world->Add(sheet, scene::Transform{});
  ctx_.world->Add(sheet, scene::Renderable{water.id});

  // Visible boulders scattered over each rock field.
  asset::Mesh boulder = asset::MakeCube(0.5f, asset::MakeAssetId("nav/boulder"));
  if (!ctx_.config->headless) ctx_.renderer->UploadMesh(boulder);
  u32 rng = 7u;
  for (const RockField& field : kRockFields) {
    for (int i = 0; i < 9; ++i) {
      const f32 angle = RandRange(rng, 0, 6.2831f);
      const f32 r = RandRange(rng, 0.3f, field.radius - 0.5f);
      const f32 x = field.x + std::cos(angle) * r;
      const f32 z = field.z + std::sin(angle) * r;
      ecs::Entity rock = ctx_.world->Create();
      scene::Transform t;
      t.position[0] = x;
      t.position[1] = SampleGround(x, z).height + 0.05f;
      t.position[2] = z;
      t.rotation[1] = std::sin(angle * 0.5f);
      t.rotation[3] = std::cos(angle * 0.5f);
      t.scale = RandRange(rng, 0.35f, 0.9f);
      ctx_.world->Add(rock, t);
      ctx_.world->Add(rock, scene::Renderable{boulder.id});
      ctx_.world->Add(rock, scene::Tint{0x8a857c});
    }
  }
}

void NavDemo::SpawnActors() {
  asset::Mesh body = asset::MakeCube(0.32f, asset::MakeAssetId("nav/agent"));
  if (!ctx_.config->headless) ctx_.renderer->UploadMesh(body);

  auto spawn = [&](f32 x, f32 z, f32 speed, u32 tint) {
    ecs::Entity e = ctx_.world->Create();
    scene::Transform t;
    t.position[0] = x;
    t.position[1] = SampleGround(x, z).height + 0.35f;
    t.position[2] = z;
    ctx_.world->Add(e, t);
    ctx_.world->Add(e, scene::Renderable{body.id});
    ctx_.world->Add(e, scene::Tint{tint});
    nav::NavAgent agent;
    agent.speed = speed;
    agent.radius = 0.3f;
    ctx_.world->Add(e, agent);
    return e;
  };

  // The porter: quick, cyan, wanders cargo routes.
  porter_ = spawn(48, 24, 4.6f, 0x30d5c8);
  // The mule pack: five pursuers fanned around the map edges.
  const f32 mule_spawns[][2] = {{8, 8}, {88, 10}, {6, 88}, {90, 86}, {48, 92}};
  const u32 mule_tints[] = {0xe06428, 0xd04848, 0xc07830, 0xb05858, 0xcc5038};
  for (u32 i = 0; i < 5; ++i) {
    mules_.push_back(spawn(mule_spawns[i][0], mule_spawns[i][1], 3.6f, mule_tints[i]));
  }
}

void NavDemo::Create() {
  // Traversal economics: rock is 3x per meter + a 4 m entry toll, water 3x +
  // 2 m. Both stay passable -- a cornered mule wades the river; it just never
  // prefers to.
  mesh_.SetAreaCost(kAreaRock, 3.0f, 4.0f);
  mesh_.SetAreaCost(kAreaWater, 3.0f, 2.0f);

  // Build the whole bubble up front (the demo world is small); the slope test
  // turns mesa flanks into holes the search must route around.
  nav::SampleFn sampler = [this](f32 x, f32 z, nav::Sample& out) {
    if (x < 1 || z < 1 || x > kWorldSize - 1 || z > kWorldSize - 1) return false;
    const Ground g = SampleGround(x, z);
    const f32 hx = SampleGround(x + 0.4f, z).height - SampleGround(x - 0.4f, z).height;
    const f32 hz = SampleGround(x, z + 0.4f).height - SampleGround(x, z - 0.4f).height;
    if ((hx * hx + hz * hz) / (0.8f * 0.8f) > 1.1f * 1.1f) return false;  // too steep
    out.height = g.height;
    out.area = g.area;
    return true;
  };
  const Vec3 center{kWorldSize * 0.5f, 0, kWorldSize * 0.5f};
  while (mesh_.EnsureBubble(center, kWorldSize * 0.75f, sampler, 64) > 0) {
  }
  RX_INFO("nav demo: {} tiles, {} cells/tile", mesh_.tile_count(),
          mesh_.config().tile_cells * mesh_.config().tile_cells);

  BuildTerrainMesh();
  SpawnActors();

  if (const char* lines_env = std::getenv("RX_NAV_LINES")) {
    draw_lines_ = lines_env[0] != '0';
  }

  // The chase: the porter wanders waypoints, the pack chases the porter. Mule
  // goals update every tick; event validation turns that into actual replans
  // only when the porter crosses a cell boundary (kGoalCellChanged), the
  // corridor is left, or a partial path runs out.
  ctx_.scheduler->AddSystem(ecs::Stage::kSim, "nav_demo", [this](ecs::World& world, f32 dt) {
    scene::Transform* porter_t = world.Get<scene::Transform>(porter_);
    nav::NavAgent* porter_agent = world.Get<nav::NavAgent>(porter_);
    if (!porter_t || !porter_agent) return;
    const Vec3 porter_pos{porter_t->position[0], porter_t->position[1], porter_t->position[2]};

    if (!porter_agent->active) {  // arrived: pick the next cargo waypoint
      const Vec3 next{RandRange(rng_, 6, kWorldSize - 6), 0, RandRange(rng_, 6, kWorldSize - 6)};
      const nav::CellRef cell = mesh_.ClampToWalkable(next, 8.0f);
      if (cell.valid()) {
        porter_agent->goal = mesh_.CellCenter(cell);
        porter_agent->active = true;
      }
    }

    for (ecs::Entity mule : mules_) {
      nav::NavAgent* agent = world.Get<nav::NavAgent>(mule);
      scene::Transform* t = world.Get<scene::Transform>(mule);
      if (!agent || !t) continue;
      agent->goal = porter_pos;
      agent->active = true;
      const f32 dx = t->position[0] - porter_pos.x;
      const f32 dz = t->position[2] - porter_pos.z;
      if (dx * dx + dz * dz < 1.2f * 1.2f) {
        // Cargo stolen: the porter respawns across the map and the hunt
        // restarts.
        ++steals_;
        const Vec3 next{RandRange(rng_, 10, kWorldSize - 10), 0,
                        RandRange(rng_, 10, kWorldSize - 10)};
        const nav::CellRef cell = mesh_.ClampToWalkable(next, 10.0f);
        if (cell.valid()) {
          const Vec3 spot = mesh_.CellCenter(cell);
          porter_t->position[0] = spot.x;
          porter_t->position[1] = spot.y + 0.35f;
          porter_t->position[2] = spot.z;
          porter_agent->active = false;
        }
      }
    }

    nav::AgentUpdateConfig config;
    config.max_repaths = 3;  // tight budget on purpose: stale corridors keep steering
    nav::UpdateAgents(world, mesh_, config, dt);
  });

  ctx_.camera->set_position({48.0f, 30.0f, 118.0f});
  ctx_.camera->set_yaw_pitch(0.0f, -0.42f);
  ctx_.camera->speed = 14.0f;
  RX_INFO("nav demo: 1 porter, {} mules; RX_NAV_LINES=0 hides the overlay", mules_.size());
}

void NavDemo::Emit(f32 dt, render::FrameView& view) {
  (void)dt;
  if (!draw_lines_ || ctx_.config->headless) return;
  lines_.clear();

  // Navmesh cells around the porter; corridors + steering for everyone.
  if (const scene::Transform* t = ctx_.world->Get<scene::Transform>(porter_)) {
    nav::AppendNavMeshLines(mesh_, {t->position[0], t->position[1], t->position[2]}, 13.0f,
                            &lines_);
  }
  ctx_.world->Each<nav::NavAgent, nav::NavCorridor, scene::Transform>(
      [&](ecs::Entity, nav::NavAgent& agent, nav::NavCorridor& state, scene::Transform& t) {
        nav::AppendCorridorLines(mesh_, state.corridor, &lines_);
        nav::AppendAgentLines(mesh_, {t.position[0], t.position[1], t.position[2]}, agent,
                              &lines_);
      });
  view.debug_lines = std::span<const render::DebugLine>(lines_.begin(), lines_.size());
}

}  // namespace rx
