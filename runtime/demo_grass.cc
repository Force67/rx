#include "demo_grass.h"

#include <algorithm>
#include <cmath>

#include "asset/primitives.h"
#include "core/log.h"
#include "scene/components.h"

namespace rx {
namespace {

constexpr f32 kOrigin = -110.0f;
constexpr f32 kExtent = 220.0f;
constexpr u32 kResolution = 221;
constexpr f32 kStep = kExtent / static_cast<f32>(kResolution - 1);

u32 PackColor(f32 r, f32 g, f32 b) {
  auto to8 = [](f32 value) {
    return static_cast<u32>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
  };
  return 0xff000000u | (to8(b) << 16u) | (to8(g) << 8u) | to8(r);
}

f32 SmoothStep(f32 a, f32 b, f32 value) {
  f32 t = std::clamp((value - a) / (b - a), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

}  // namespace

f32 GrassDemo::TerrainHeight(f32 x, f32 z) const {
  f32 broad = std::sin(x * 0.025f) * 4.8f + std::cos(z * 0.031f) * 3.6f;
  f32 crossing = std::sin((x + z) * 0.019f + 0.8f) * 3.1f;
  f32 detail = std::sin(x * 0.071f - z * 0.049f) * 0.75f +
               std::cos(x * 0.043f + z * 0.063f) * 0.55f;
  return broad + crossing + detail;
}

void GrassDemo::BuildField() {
  types_.resize(3);
  render::GrassType& meadow = types_[0];
  meadow.base_color[0] = 0.028f;
  meadow.base_color[1] = 0.16f;
  meadow.base_color[2] = 0.016f;
  meadow.base_color[3] = 0.14f;
  meadow.tip_color[0] = 0.20f;
  meadow.tip_color[1] = 0.52f;
  meadow.tip_color[2] = 0.065f;
  meadow.tip_color[3] = 0.42f;
  meadow.dimensions[0] = 0.65f;
  meadow.dimensions[1] = 1.28f;
  meadow.dimensions[2] = 0.060f;
  meadow.dimensions[3] = 0.120f;
  meadow.shape[0] = 0.06f;
  meadow.shape[1] = 0.36f;
  meadow.shape[2] = 0.32f;
  meadow.shape[3] = 0.10f;
  meadow.material[0] = 0.82f;
  meadow.material[1] = 1.0f;
  meadow.material[2] = 2.8f;
  meadow.material[3] = 0.38f;

  render::GrassType& gold = types_[1];
  gold.base_color[0] = 0.16f;
  gold.base_color[1] = 0.095f;
  gold.base_color[2] = 0.014f;
  gold.base_color[3] = 0.13f;
  gold.tip_color[0] = 0.46f;
  gold.tip_color[1] = 0.31f;
  gold.tip_color[2] = 0.052f;
  gold.tip_color[3] = 0.56f;
  gold.dimensions[0] = 0.86f;
  gold.dimensions[1] = 1.58f;
  gold.dimensions[2] = 0.055f;
  gold.dimensions[3] = 0.110f;
  gold.shape[0] = 0.12f;
  gold.shape[1] = 0.50f;
  gold.shape[2] = 0.40f;
  gold.shape[3] = 0.15f;
  gold.material[0] = 0.76f;
  gold.material[1] = 1.28f;
  gold.material[2] = 4.2f;
  gold.material[3] = 0.34f;

  render::GrassType& sage = types_[2];
  sage.base_color[0] = 0.045f;
  sage.base_color[1] = 0.14f;
  sage.base_color[2] = 0.042f;
  sage.base_color[3] = 0.11f;
  sage.tip_color[0] = 0.24f;
  sage.tip_color[1] = 0.40f;
  sage.tip_color[2] = 0.12f;
  sage.tip_color[3] = 0.32f;
  sage.dimensions[0] = 0.50f;
  sage.dimensions[1] = 1.02f;
  sage.dimensions[2] = 0.065f;
  sage.dimensions[3] = 0.130f;
  sage.shape[0] = 0.04f;
  sage.shape[1] = 0.27f;
  sage.shape[2] = 0.22f;
  sage.shape[3] = 0.08f;
  sage.material[0] = 0.88f;
  sage.material[1] = 0.72f;
  sage.material[2] = 2.2f;
  sage.material[3] = 0.42f;

  samples_.resize(static_cast<size_t>(kResolution) * kResolution);
  for (u32 z = 0; z < kResolution; ++z) {
    for (u32 x = 0; x < kResolution; ++x) {
      const f32 world_x = kOrigin + static_cast<f32>(x) * kStep;
      const f32 world_z = kOrigin + static_cast<f32>(z) * kStep;
      render::GrassFieldSample& sample = samples_[z * kResolution + x];
      sample.height = TerrainHeight(world_x, world_z);
      const f32 patch = 0.5f + 0.25f * std::sin(world_x * 0.057f + world_z * 0.031f) +
                        0.20f * std::cos(world_x * 0.021f - world_z * 0.069f);
      const f32 path_center = std::sin(world_x * 0.035f) * 7.0f - 4.0f;
      const f32 path_distance = std::fabs(world_z - path_center);
      const f32 path_mask = SmoothStep(1.2f, 4.5f, path_distance);
      sample.density = std::clamp((0.90f + patch * 0.14f) * path_mask, 0.0f, 1.0f);
      sample.growth = 0.90f + patch * 0.34f;
      const bool golden_ridge = sample.height > 4.2f && patch > 0.5f;
      const bool cool_hollow = sample.height < -2.4f || patch < 0.27f;
      sample.type = golden_ridge ? 1u : (cool_hollow ? 2u : 0u);
    }
  }

  domain_.samples = samples_.data();
  domain_.sample_width = kResolution;
  domain_.sample_height = kResolution;
  domain_.origin_x = kOrigin;
  domain_.origin_z = kOrigin;
  domain_.extent_x = kExtent;
  domain_.extent_z = kExtent;
  domain_.types = types_.data();
  domain_.type_count = static_cast<u32>(types_.size());
  domain_.seed = 0x7a6b5c4du;
  domain_.settings.candidate_spacing = 0.30f;
  domain_.settings.stream_tile_size = 18.0f;
  domain_.settings.stream_radius = 84.0f;
  domain_.settings.density_lod_start = 40.0f;
  domain_.settings.density_lod_end = 76.0f;
  domain_.settings.far_density = 0.46f;
  domain_.settings.geometry_lod_start = 18.0f;
  domain_.settings.geometry_lod_end = 48.0f;
  domain_.settings.fade_start = 76.0f;
  domain_.settings.fade_end = 84.0f;
  domain_.settings.max_slope_cos = 0.50f;
  domain_.settings.max_blades = 210000;
}

void GrassDemo::BuildTerrain() {
  asset::Material terrain_material;
  terrain_material.id = asset::MakeAssetId("grass/terrain_material");
  terrain_material.base_color_factor[0] = 1.0f;
  terrain_material.base_color_factor[1] = 1.0f;
  terrain_material.base_color_factor[2] = 1.0f;
  terrain_material.roughness_factor = 0.96f;

  asset::Mesh terrain;
  terrain.id = asset::MakeAssetId("grass/hills");
  asset::MeshLod& lod = terrain.lods.emplace_back();
  lod.vertices.reserve(static_cast<size_t>(kResolution) * kResolution);
  for (u32 z = 0; z < kResolution; ++z) {
    for (u32 x = 0; x < kResolution; ++x) {
      const f32 world_x = kOrigin + static_cast<f32>(x) * kStep;
      const f32 world_z = kOrigin + static_cast<f32>(z) * kStep;
      const render::GrassFieldSample& sample = samples_[z * kResolution + x];
      asset::Vertex vertex{};
      vertex.position[0] = world_x;
      vertex.position[1] = sample.height;
      vertex.position[2] = world_z;
      const f32 hx0 = TerrainHeight(world_x - kStep, world_z);
      const f32 hx1 = TerrainHeight(world_x + kStep, world_z);
      const f32 hz0 = TerrainHeight(world_x, world_z - kStep);
      const f32 hz1 = TerrainHeight(world_x, world_z + kStep);
      Vec3 normal = Normalize(Vec3{hx0 - hx1, 2.0f * kStep, hz0 - hz1});
      vertex.normal[0] = normal.x;
      vertex.normal[1] = normal.y;
      vertex.normal[2] = normal.z;
      vertex.tangent[0] = 1.0f;
      vertex.tangent[3] = 1.0f;
      vertex.uv[0] = world_x * 0.08f;
      vertex.uv[1] = world_z * 0.08f;
      f32 r = 0.10f, g = 0.22f, b = 0.055f;
      if (sample.type == 1u) {
        r = 0.20f;
        g = 0.18f;
        b = 0.05f;
      } else if (sample.type == 2u) {
        r = 0.12f;
        g = 0.20f;
        b = 0.10f;
      }
      const f32 bare = 1.0f - sample.density;
      r = r + (0.24f - r) * bare;
      g = g + (0.17f - g) * bare;
      b = b + (0.08f - b) * bare;
      vertex.color = PackColor(r, g, b);
      lod.vertices.push_back(vertex);
    }
  }
  for (u32 z = 0; z + 1 < kResolution; ++z) {
    for (u32 x = 0; x + 1 < kResolution; ++x) {
      const u32 a = z * kResolution + x;
      const u32 b = a + 1;
      const u32 c = a + kResolution;
      const u32 d = c + 1;
      for (u32 index : {a, c, b, b, c, d})
        lod.indices.push_back(index);
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), terrain_material.id});
  terrain.bounds_radius = kExtent;

  if (!ctx_.config->headless) {
    ctx_.renderer->UploadMaterial(terrain_material);
    ctx_.renderer->UploadMesh(terrain);
  }
  ecs::Entity entity = ctx_.world->Create();
  ctx_.world->Add(entity, scene::Transform{});
  ctx_.world->Add(entity, scene::Renderable{terrain.id});
}

void GrassDemo::BuildGrowableStone() {
  constexpr f32 kX = 12.0f;
  constexpr f32 kZ = -5.0f;
  constexpr f32 kHalfX = 3.2f;
  constexpr f32 kHalfZ = 2.1f;
  constexpr f32 kHalfY = 0.55f;
  const f32 center_y = TerrainHeight(kX, kZ) + kHalfY * 0.72f;
  const f32 top = center_y + kHalfY;

  asset::Material stone_material;
  stone_material.id = asset::MakeAssetId("grass/growable_stone_material");
  stone_material.base_color_factor[0] = 0.22f;
  stone_material.base_color_factor[1] = 0.24f;
  stone_material.base_color_factor[2] = 0.19f;
  stone_material.roughness_factor = 0.92f;
  asset::Mesh stone =
      asset::MakeBox(kHalfX, kHalfY, kHalfZ, asset::MakeAssetId("grass/growable_stone"));
  if (stone.lods[0].submeshes.empty()) {
    stone.lods[0].submeshes.push_back(
        {0, static_cast<u32>(stone.lods[0].indices.size()), stone_material.id});
  } else {
    stone.lods[0].submeshes[0].material = stone_material.id;
  }
  if (!ctx_.config->headless) {
    ctx_.renderer->UploadMaterial(stone_material);
    ctx_.renderer->UploadMesh(stone);
  }
  ecs::Entity entity = ctx_.world->Create();
  ctx_.world->Add(entity, scene::Transform{.position = {kX, center_y, kZ}});
  ctx_.world->Add(entity, scene::Renderable{stone.id});

  surfaces_.resize(2);
  auto set_point = [](f32 point[3], f32 x, f32 y, f32 z) {
    point[0] = x;
    point[1] = y;
    point[2] = z;
  };
  render::GrassSurfaceTriangle& first = surfaces_[0];
  set_point(first.p0, kX - kHalfX, top, kZ - kHalfZ);
  set_point(first.p1, kX - kHalfX, top, kZ + kHalfZ);
  set_point(first.p2, kX + kHalfX, top, kZ - kHalfZ);
  first.density = 0.82f;
  first.growth = 0.82f;
  first.type = 2;
  first.surface_id = 0x51a7u;
  render::GrassSurfaceTriangle& second = surfaces_[1];
  set_point(second.p0, kX + kHalfX, top, kZ - kHalfZ);
  set_point(second.p1, kX - kHalfX, top, kZ + kHalfZ);
  set_point(second.p2, kX + kHalfX, top, kZ + kHalfZ);
  second.density = 0.82f;
  second.growth = 0.82f;
  second.type = 2;
  second.surface_id = 0x51a8u;
  domain_.surfaces = surfaces_.data();
  domain_.surface_count = static_cast<u32>(surfaces_.size());
}

void GrassDemo::Create() {
  BuildField();
  BuildTerrain();
  BuildGrowableStone();

  asset::Material marker_material;
  marker_material.id = asset::MakeAssetId("grass/interaction_marker_material");
  marker_material.base_color_factor[0] = 0.48f;
  marker_material.base_color_factor[1] = 0.12f;
  marker_material.base_color_factor[2] = 0.045f;
  marker_material.roughness_factor = 0.55f;
  asset::Mesh marker =
      asset::MakeSphere(0.55f, 20, 28, asset::MakeAssetId("grass/interaction_marker"));
  marker.lods[0].submeshes[0].material = marker_material.id;
  if (!ctx_.config->headless) {
    ctx_.renderer->UploadMaterial(marker_material);
    ctx_.renderer->UploadMesh(marker);
  }
  interaction_marker_ = ctx_.world->Create();
  ctx_.world->Add(interaction_marker_, scene::Transform{});
  ctx_.world->Add(interaction_marker_, scene::Renderable{marker.id});

  ctx_.scene_owns_sun = true;
  render::RenderSettings& settings = ctx_.renderer->settings();
  settings.sun_direction = {-0.42f, -0.78f, -0.34f};
  settings.sun_color = {1.0f, 0.88f, 0.68f};
  settings.sun_intensity = 4.8f;
  settings.ambient = 0.10f;
  settings.night = 0.0f;
  settings.weather.wind_speed = 8.5f;
  settings.weather.wind_yaw = 0.42f;
  settings.weather.gustiness = 0.82f;
  settings.motion_blur = false;
  settings.dof = false;
  settings.clouds = false;
  settings.aerial_perspective = 0.45f;
  settings.lens_flare = 0.025f;

  ctx_.camera->set_position({-36.0f, 9.5f, 30.0f});
  ctx_.camera->set_yaw_pitch(0.78f, -0.18f);
  ctx_.camera->speed = 10.0f;
  RX_INFO("procedural grass demo: semantic hills, growable stone and local interaction");
}

void GrassDemo::Update(f32 dt) {
  time_ += dt;
  const f32 x = std::sin(time_ * 0.34f) * 12.0f - 3.0f;
  const f32 z = std::cos(time_ * 0.27f) * 7.0f + 4.0f;
  const f32 y = TerrainHeight(x, z) + 0.62f;
  interaction_position_ = {x, y, z};
  interaction_direction_ = {std::cos(time_ * 0.34f), 0.0f, -std::sin(time_ * 0.27f)};
  if (scene::Transform* transform =
          ctx_.world->Get<scene::Transform>(interaction_marker_)) {
    transform->position[0] = x;
    transform->position[1] = y;
    transform->position[2] = z;
  }
}

void GrassDemo::Emit(render::FrameView& view) {
  view.grass_domain = &domain_;
  render::GrassInteraction interaction;
  interaction.position_radius[0] = interaction_position_.x;
  interaction.position_radius[1] = interaction_position_.y;
  interaction.position_radius[2] = interaction_position_.z;
  interaction.position_radius[3] = 3.2f;
  interaction.direction_strength[0] = interaction_direction_.x;
  interaction.direction_strength[1] = interaction_direction_.y;
  interaction.direction_strength[2] = interaction_direction_.z;
  interaction.direction_strength[3] = 0.95f;
  view.grass_interactions.push_back(interaction);
}

}  // namespace rx
