#include "demo_drive.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "asset/asset_id.h"
#include "asset/gltf_loader.h"
#include "asset/material.h"
#include "asset/mesh.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "ecs/world.h"
#include "scene/components.h"

#if defined(RX_HAS_IMGUI)
#include <imgui.h>
#endif

// The driving gym: a material heightfield with a road loop, a runway, an ice
// patch and a lake; a car (CesiumMilkTruck), a boat (procedural hull) and a
// plane (Cesium_Air) driven through engine/physics; procedural VehicleAudio on
// each; and ToyCar/CarConcept/GroundVehicle parked as PBR showcase pieces. The
// demo owns its camera + input like the gym and steps the vehicles on the
// scheduler's kPreSim stage, before the physics world advances.
namespace rx {
namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kHalfPi = kPi * 0.5f;

// --- terrain layout (world XZ, metres) -------------------------------------
constexpr f32 kTerrainSize = 400.0f;
constexpr f32 kTerrainMin = -200.0f;  // min corner of the heightfield
constexpr u32 kSamples = 256;         // heightfield / render grid resolution
constexpr f32 kLakeCx = 120.0f;
constexpr f32 kLakeCz = 120.0f;
constexpr f32 kLakeY = -2.0f;             // flat water surface over the lake
constexpr f32 kLakeWaterRadius = 96.0f;   // set_water_height returns true within

// Palette indices, shared by the physics material heightfield and the render
// mesh submeshes so a region's grip and its colour agree.
enum Region : u8 { kGrass = 0, kAsphalt = 1, kIce = 2, kDirt = 3, kSand = 4, kRegionCount = 5 };

const physics::SurfaceType kPalette[kRegionCount] = {
    physics::SurfaceType::kGrass, physics::SurfaceType::kAsphalt, physics::SurfaceType::kIce,
    physics::SurfaceType::kDirt, physics::SurfaceType::kSand};

f32 SmoothStep(f32 t) {
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// 1 for v inside [lo,hi], easing to 0 over `feather` metres past each edge.
f32 Feather(f32 v, f32 lo, f32 hi, f32 feather) {
  f32 a = SmoothStep((v - (lo - feather)) / feather);
  f32 b = SmoothStep(((hi + feather) - v) / feather);
  return std::min(a, b);
}

u8 RegionAt(f32 x, f32 z) {
  // Runway: a long asphalt strip on the -X side.
  if (x >= -74.0f && x <= -50.0f && z >= -152.0f && z <= 152.0f) return kAsphalt;
  // Road loop centred on the origin (a square ring band).
  const f32 m = std::max(std::fabs(x), std::fabs(z));
  if (m >= 40.0f && m <= 57.0f) {
    if (x >= 40.0f && x <= 57.0f && std::fabs(z) <= 9.0f) return kIce;  // ice on the east straight
    return kAsphalt;
  }
  // Dirt field just inside the loop's north-west.
  if (x >= -46.0f && x <= -12.0f && z >= 40.0f && z <= 84.0f) return kDirt;
  // Sand beach ringing the lake.
  const f32 ld = std::sqrt((x - kLakeCx) * (x - kLakeCx) + (z - kLakeCz) * (z - kLakeCz));
  if (ld >= 66.0f && ld <= 92.0f) return kSand;
  return kGrass;
}

f32 HeightAt(f32 x, f32 z) {
  f32 h = 1.8f * std::sin(x * 0.017f) * std::cos(z * 0.019f) +
          1.1f * std::sin((x + z) * 0.011f + 0.6f);
  // Lake basin: dip below the water surface toward the centre.
  const f32 ld = std::sqrt((x - kLakeCx) * (x - kLakeCx) + (z - kLakeCz) * (z - kLakeCz));
  if (ld < 104.0f) {
    const f32 t = SmoothStep((104.0f - ld) / (104.0f - 40.0f));
    h = h + (-6.0f - h) * t;
  }
  // Flatten the road loop and the runway so the wheeled vehicles roll on level
  // ground and the plane has a real runway.
  const f32 m = std::max(std::fabs(x), std::fabs(z));
  const f32 road_w = Feather(m, 40.0f, 57.0f, 3.0f);
  h = h + (0.0f - h) * road_w;
  const f32 rw = Feather(x, -74.0f, -50.0f, 3.0f) * Feather(z, -152.0f, 152.0f, 4.0f);
  h = h + (0.0f - h) * rw;
  return h;
}

const char* SurfaceName(physics::SurfaceType s) {
  switch (s) {
    case physics::SurfaceType::kAsphalt: return "asphalt";
    case physics::SurfaceType::kConcrete: return "concrete";
    case physics::SurfaceType::kDirt: return "dirt";
    case physics::SurfaceType::kGravel: return "gravel";
    case physics::SurfaceType::kGrass: return "grass";
    case physics::SurfaceType::kSand: return "sand";
    case physics::SurfaceType::kSnow: return "snow";
    case physics::SurfaceType::kIce: return "ice";
    case physics::SurfaceType::kMud: return "mud";
    case physics::SurfaceType::kWood: return "wood";
    case physics::SurfaceType::kMetal: return "metal";
    default: return "?";
  }
}

// Non-uniform scale matrix (column major).
Mat4 ScaleMat(const Vec3& s) {
  Mat4 m = Mat4::Identity();
  m.m[0] = s.x;
  m.m[5] = s.y;
  m.m[10] = s.z;
  return m;
}

// --- small procedural-mesh helpers (boat hull + wheel) ---------------------
struct MeshBuild {
  base::Vector<asset::Vertex> v;
  base::Vector<u32> i;
  u32 Add(const Vec3& p, f32 u, f32 tv) {
    asset::Vertex vert{};
    vert.position[0] = p.x;
    vert.position[1] = p.y;
    vert.position[2] = p.z;
    vert.tangent[0] = 1;
    vert.tangent[3] = 1;
    vert.uv[0] = u;
    vert.uv[1] = tv;
    vert.color = 0xffffffff;
    v.push_back(vert);
    return static_cast<u32>(v.size() - 1);
  }
  void Tri(u32 a, u32 b, u32 c) {
    i.push_back(a);
    i.push_back(b);
    i.push_back(c);
  }
  void Quad(u32 a, u32 b, u32 c, u32 d) {
    Tri(a, b, c);
    Tri(a, c, d);
  }
};

void ComputeSmoothNormals(MeshBuild& mb) {
  for (asset::Vertex& vert : mb.v) vert.normal[0] = vert.normal[1] = vert.normal[2] = 0;
  for (u32 t = 0; t + 2 < mb.i.size(); t += 3) {
    u32 ia = mb.i[t], ib = mb.i[t + 1], ic = mb.i[t + 2];
    Vec3 pa{mb.v[ia].position[0], mb.v[ia].position[1], mb.v[ia].position[2]};
    Vec3 pb{mb.v[ib].position[0], mb.v[ib].position[1], mb.v[ib].position[2]};
    Vec3 pc{mb.v[ic].position[0], mb.v[ic].position[1], mb.v[ic].position[2]};
    Vec3 n = Cross(pb - pa, pc - pa);
    for (u32 idx : {ia, ib, ic}) {
      mb.v[idx].normal[0] += n.x;
      mb.v[idx].normal[1] += n.y;
      mb.v[idx].normal[2] += n.z;
    }
  }
  for (asset::Vertex& vert : mb.v) {
    Vec3 n = Normalize(Vec3{vert.normal[0], vert.normal[1], vert.normal[2]});
    vert.normal[0] = n.x;
    vert.normal[1] = n.y;
    vert.normal[2] = n.z;
  }
}

asset::Mesh FinishMesh(MeshBuild& mb, const char* id, asset::AssetId material) {
  ComputeSmoothNormals(mb);
  asset::Mesh mesh;
  mesh.id = asset::MakeAssetId(id);
  mesh.lods.emplace_back();
  asset::MeshLod& lod = mesh.lods[0];
  lod.vertices = mb.v;
  lod.indices = mb.i;
  lod.submeshes.push_back({0, static_cast<u32>(mb.i.size()), material});
  f32 r = 0;
  for (const asset::Vertex& vert : mb.v)
    r = std::max(r, std::sqrt(vert.position[0] * vert.position[0] +
                              vert.position[1] * vert.position[1] +
                              vert.position[2] * vert.position[2]));
  mesh.bounds_radius = r;
  return mesh;
}

// Solid diffuse material, uploaded unless headless.
asset::Material SolidMaterial(EngineContext& ctx, const char* id, Vec3 color, f32 rough,
                              f32 metal = 0.0f) {
  asset::Material m;
  m.id = asset::MakeAssetId(id);
  m.base_color_factor[0] = color.x;
  m.base_color_factor[1] = color.y;
  m.base_color_factor[2] = color.z;
  m.base_color_factor[3] = 1.0f;
  m.roughness_factor = rough;
  m.metallic_factor = metal;
  if (!ctx.config->headless) ctx.renderer->UploadMaterial(m);
  return m;
}

}  // namespace

DriveDemo::DriveDemo(EngineContext& ctx) : ctx_(ctx) {}

void DriveDemo::Create() {
  // Fixed afternoon sun, like the ship/gym demos, so captures are frame-stable.
  if (!ctx_.config->headless) {
    auto& s = ctx_.renderer->settings();
    s.sun_direction = Normalize(Vec3{-0.4f, -0.82f, -0.35f});
    s.sun_intensity = 4.4f;
    s.sun_color = {1.0f, 0.96f, 0.9f};
    s.ambient = 0.24f;
    s.night = 0.0f;
    s.lens_flare = 0.0f;
  }
  ctx_.scene_owns_sun = true;

  BuildTerrain();
  BuildWater();
  BuildWheelMesh();
  BuildBoatHull();

  // Vendor models: the driveable car + plane visuals, and the parked showcase.
  LoadModel("assets/vehicles/CesiumMilkTruck.glb", /*skip_mesh_index=*/0, &car_model_);
  LoadModel("assets/vehicles/Cesium_Air.glb", -1, &plane_model_);
  if (car_model_.loaded) car_norm_ = NormalizeXform(car_model_, 4.0f, 0.0f, false);
  if (plane_model_.loaded) plane_norm_ = NormalizeXform(plane_model_, 6.0f, 0.0f, false);

  // Graybox fallbacks (also the boat's material palette) for missing assets.
  {
    asset::Material red = SolidMaterial(ctx_, "drive/graybox_mat", {0.55f, 0.09f, 0.07f}, 0.5f);
    asset::Mesh box = asset::MakeCube(1.0f, asset::MakeAssetId("drive/graybox"));
    for (asset::MeshLod& l : box.lods) {
      if (l.submeshes.empty())
        l.submeshes.push_back({0, static_cast<u32>(l.indices.size()), red.id});
      else
        l.submeshes[0].material = red.id;
    }
    if (!ctx_.config->headless) ctx_.renderer->UploadMesh(box);
    graybox_car_ = box.id.hash;
    graybox_plane_ = box.id.hash;
  }

  struct ShowcasePiece {
    const char* path;
    f32 x;
    f32 len;
  };
  const ShowcasePiece pieces[3] = {
      {"assets/vehicles/ToyCar.glb", -16.0f, 3.2f},
      {"assets/vehicles/CarConcept.glb", 0.0f, 4.4f},
      {"assets/vehicles/GroundVehicle.glb", 16.0f, 4.2f},
  };
  for (const ShowcasePiece& p : pieces) {
    Model m;
    if (!LoadModel(p.path, -1, &m)) continue;
    // Parked on the north straight (asphalt), facing the infield.
    const f32 gy = HeightAt(p.x, 48.0f);
    const Mat4 place = MakeTranslation({p.x, gy, 48.0f}) *
                       MakeFromQuat(QuatFromAxisAngle({0, 1, 0}, kPi));
    const Mat4 norm = NormalizeXform(m, p.len, 0.0f, /*sit_on_ground=*/true);
    showcase_.push_back({std::move(m), place * norm});
  }

  SpawnVehicles();
  SetupAudio();

  // Physics-coupled vehicle stepping: stage this step's inputs/forces BEFORE the
  // world advances (kPreSim runs ahead of the host's kSim physics system).
  if (!registered_ && ctx_.scheduler) {
    ctx_.scheduler->AddSystem(ecs::Stage::kPreSim, "drive_vehicles",
                              [this](ecs::World&, f32 dt) { StepVehicles(dt); });
    registered_ = true;
  }

  RX_INFO(
      "drive demo: Tab cycle car/boat/plane, WASD drive/steer, arrows fly, Space handbrake/brakes, "
      "M manual, Shift/Ctrl shift, F flaps, J rain, R reset, C chase/free camera");
}

void DriveDemo::BuildTerrain() {
  // Shared height + region sampling: build the physics heightfield and the
  // render mesh from the same functions so grip regions line up with colour.
  const f32 spacing = kTerrainSize / static_cast<f32>(kSamples - 1);

  base::Vector<f32> heights;
  heights.resize(kSamples * kSamples);
  for (u32 z = 0; z < kSamples; ++z) {
    for (u32 x = 0; x < kSamples; ++x) {
      const f32 wx = kTerrainMin + x * spacing;
      const f32 wz = kTerrainMin + z * spacing;
      heights[z * kSamples + x] = HeightAt(wx, wz);
    }
  }

  // Per-quad material index (row-major, matching Jolt's height-field layout).
  std::vector<u8> mat_idx((kSamples - 1) * (kSamples - 1));
  for (u32 z = 0; z + 1 < kSamples; ++z) {
    for (u32 x = 0; x + 1 < kSamples; ++x) {
      const f32 wx = kTerrainMin + (x + 0.5f) * spacing;
      const f32 wz = kTerrainMin + (z + 0.5f) * spacing;
      mat_idx[z * (kSamples - 1) + x] = RegionAt(wx, wz);
    }
  }
  ctx_.physics->AddHeightField({kTerrainMin, 0.0f, kTerrainMin}, heights.data(), kSamples,
                               kTerrainSize, mat_idx.data(), kPalette, kRegionCount);

  // Region materials (index-aligned with kPalette).
  asset::Material region_mats[kRegionCount] = {
      SolidMaterial(ctx_, "drive/grass", {0.14f, 0.30f, 0.11f}, 0.95f),
      SolidMaterial(ctx_, "drive/asphalt", {0.07f, 0.07f, 0.08f}, 0.85f),
      SolidMaterial(ctx_, "drive/ice", {0.68f, 0.80f, 0.90f}, 0.12f),
      SolidMaterial(ctx_, "drive/dirt", {0.33f, 0.23f, 0.13f}, 0.95f),
      SolidMaterial(ctx_, "drive/sand", {0.76f, 0.68f, 0.45f}, 0.9f),
  };

  // Render mesh: one shared vertex grid, per-region index buckets -> submeshes.
  asset::Mesh terrain;
  terrain.id = asset::MakeAssetId("drive/terrain");
  terrain.lods.emplace_back();
  asset::MeshLod& lod = terrain.lods[0];
  lod.vertices.resize(kSamples * kSamples);
  for (u32 z = 0; z < kSamples; ++z) {
    for (u32 x = 0; x < kSamples; ++x) {
      const f32 wx = kTerrainMin + x * spacing;
      const f32 wz = kTerrainMin + z * spacing;
      const f32 hl = HeightAt(wx - spacing, wz);
      const f32 hr = HeightAt(wx + spacing, wz);
      const f32 hd = HeightAt(wx, wz - spacing);
      const f32 hu = HeightAt(wx, wz + spacing);
      Vec3 n = Normalize(Vec3{(hl - hr), 2.0f * spacing, (hd - hu)});
      asset::Vertex& v = lod.vertices[z * kSamples + x];
      v.position[0] = wx;
      v.position[1] = heights[z * kSamples + x];
      v.position[2] = wz;
      v.normal[0] = n.x;
      v.normal[1] = n.y;
      v.normal[2] = n.z;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.uv[0] = wx * 0.25f;
      v.uv[1] = wz * 0.25f;
      v.color = 0xffffffff;
    }
  }
  std::vector<u32> buckets[kRegionCount];
  for (u32 z = 0; z + 1 < kSamples; ++z) {
    for (u32 x = 0; x + 1 < kSamples; ++x) {
      const u8 r = mat_idx[z * (kSamples - 1) + x];
      const u32 a = z * kSamples + x;
      const u32 b = a + 1;
      const u32 c = a + kSamples;
      const u32 d = c + 1;
      std::vector<u32>& bk = buckets[r];
      for (u32 idx : {a, b, c, b, d, c}) bk.push_back(idx);
    }
  }
  for (u32 r = 0; r < kRegionCount; ++r) {
    if (buckets[r].empty()) continue;
    const u32 offset = static_cast<u32>(lod.indices.size());
    for (u32 idx : buckets[r]) lod.indices.push_back(idx);
    lod.submeshes.push_back({offset, static_cast<u32>(buckets[r].size()), region_mats[r].id});
  }
  terrain.bounds_center[1] = -3.0f;
  terrain.bounds_radius = kTerrainSize;
  if (!ctx_.config->headless) ctx_.renderer->UploadMesh(terrain);
  ecs::Entity e = ctx_.world->Create();
  ctx_.world->Add(e, scene::Transform{});
  ctx_.world->Add(e, scene::Renderable{terrain.id});
}

void DriveDemo::BuildWater() {
  // Rendered water sheet at kLakeY over the lake, matching CreateWaterDemoScene:
  // a subdivided is_water grid the mesh.vs gerstner path displaces.
  asset::Material water_mat;
  water_mat.id = asset::MakeAssetId("drive/water_mat");
  water_mat.base_color_factor[0] = 0.06f;
  water_mat.base_color_factor[1] = 0.12f;
  water_mat.base_color_factor[2] = 0.17f;
  water_mat.base_color_factor[3] = 0.75f;
  water_mat.metallic_factor = 0;
  water_mat.roughness_factor = 0.05f;
  water_mat.alpha_mode = asset::AlphaMode::kBlend;
  water_mat.two_sided = true;
  water_mat.is_water = true;

  asset::Mesh water;
  water.id = asset::MakeAssetId("drive/water");
  water.lods.emplace_back();
  asset::MeshLod& lod = water.lods[0];
  constexpr f32 kHalf = 110.0f;
  constexpr u32 kGrid = 200;
  for (u32 gy = 0; gy <= kGrid; ++gy) {
    for (u32 gx = 0; gx <= kGrid; ++gx) {
      asset::Vertex v{};
      v.position[0] = kLakeCx - kHalf + 2.0f * kHalf * static_cast<f32>(gx) / kGrid;
      v.position[1] = 0;
      v.position[2] = kLakeCz - kHalf + 2.0f * kHalf * static_cast<f32>(gy) / kGrid;
      v.normal[1] = 1;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.uv[0] = v.position[0] / 8.0f;
      v.uv[1] = v.position[2] / 8.0f;
      v.color = 0xffffffff;
      lod.vertices.push_back(v);
    }
  }
  for (u32 gy = 0; gy < kGrid; ++gy) {
    for (u32 gx = 0; gx < kGrid; ++gx) {
      u32 a = gy * (kGrid + 1) + gx, b = a + 1, c = a + (kGrid + 1), d = c + 1;
      for (u32 idx : {a, b, c, b, d, c}) lod.indices.push_back(idx);
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), water_mat.id});
  water.bounds_center[0] = kLakeCx;
  water.bounds_center[2] = kLakeCz;
  water.bounds_radius = kHalf * 1.5f;
  if (!ctx_.config->headless) {
    ctx_.renderer->UploadMaterial(water_mat);
    ctx_.renderer->UploadMesh(water);
  }
  ecs::Entity sheet = ctx_.world->Create();
  ctx_.world->Add(sheet, scene::Transform{.position = {0, kLakeY, 0}});
  ctx_.world->Add(sheet, scene::Renderable{water.id});

  // Physics water: flat surface over the lake only, so the boat floats there and
  // the car's tires never read the dry world as submerged.
  ctx_.physics->set_water_height([](const Vec3& p, f32* height, Vec3* flow) {
    const f32 d =
        std::sqrt((p.x - kLakeCx) * (p.x - kLakeCx) + (p.z - kLakeCz) * (p.z - kLakeCz));
    if (d > kLakeWaterRadius) return false;
    *height = kLakeY;
    if (flow) *flow = {};
    return true;
  });
}

void DriveDemo::BuildWheelMesh() {
  asset::Material tire = SolidMaterial(ctx_, "drive/tire_mat", {0.05f, 0.05f, 0.06f}, 0.75f);
  // Unit cylinder around Y, radius 1, height 1 (y in [-0.5, 0.5]).
  asset::Mesh mesh;
  mesh.id = asset::MakeAssetId("drive/wheel");
  mesh.lods.emplace_back();
  asset::MeshLod& lod = mesh.lods[0];
  const u32 seg = 20;
  for (u32 i = 0; i <= seg; ++i) {
    const f32 t = static_cast<f32>(i) / seg;
    const f32 a = t * 2.0f * kPi;
    for (int k = 0; k < 2; ++k) {
      asset::Vertex v{};
      v.position[0] = std::cos(a);
      v.position[1] = k ? 0.5f : -0.5f;
      v.position[2] = std::sin(a);
      v.normal[0] = std::cos(a);
      v.normal[2] = std::sin(a);
      v.tangent[0] = -std::sin(a);
      v.tangent[2] = std::cos(a);
      v.tangent[3] = 1;
      v.uv[0] = t * 4.0f;
      v.uv[1] = k ? 1.0f : 0.0f;
      lod.vertices.push_back(v);
    }
  }
  for (u32 i = 0; i < seg; ++i) {
    const u32 a = i * 2, b = a + 1, c = a + 2, d = a + 3;
    for (u32 idx : {a, c, b, b, c, d}) lod.indices.push_back(idx);
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), tire.id});
  mesh.bounds_radius = 1.2f;
  if (!ctx_.config->headless) ctx_.renderer->UploadMesh(mesh);
  wheel_mesh_ = mesh.id.hash;
}

void DriveDemo::BuildBoatHull() {
  asset::Material paint = SolidMaterial(ctx_, "drive/hull_mat", {0.6f, 0.62f, 0.66f}, 0.3f, 0.1f);
  // Lofted U-profile hull, boat-local: waterline at y=0, keel below, deck above,
  // pointed bow at +Z. Sized to the BoatDesc default hull (0.9 x 0.5 x 2.9).
  constexpr f32 kHL = 2.9f, kHB = 0.9f, kDeck = 0.5f, kKeel = -0.5f;
  constexpr u32 kSt = 18, kPr = 10;
  MeshBuild hb;
  auto width_scale = [](f32 z) -> f32 {
    const f32 tn = z / kHL;
    if (tn > 0) return std::max(0.06f, 1.0f - tn * tn * 0.97f);  // pointed bow
    return std::max(0.55f, 1.0f - tn * tn * 0.4f);               // fuller transom
  };
  for (u32 si = 0; si <= kSt; ++si) {
    const f32 z = -kHL + 2.0f * kHL * static_cast<f32>(si) / kSt;
    const f32 ws = width_scale(z);
    for (u32 pj = 0; pj <= kPr; ++pj) {
      const f32 th = kPi * static_cast<f32>(pj) / kPr;
      const f32 x = -kHB * ws * std::cos(th);
      const f32 y = kDeck - (kDeck - kKeel) * std::pow(std::sin(th), 0.62f);
      hb.Add({x, y, z}, static_cast<f32>(pj) / kPr, z / 4.0f);
    }
  }
  const u32 row = kPr + 1;
  for (u32 si = 0; si < kSt; ++si)
    for (u32 pj = 0; pj < kPr; ++pj) {
      const u32 a = si * row + pj;
      hb.Quad(a, a + 1, a + row + 1, a + row);
    }
  // Flat deck across the top rails, and a stern transom fan.
  {
    base::Vector<u32> dp, ds;
    for (u32 si = 0; si <= kSt; ++si) {
      const f32 z = -kHL + 2.0f * kHL * static_cast<f32>(si) / kSt;
      const f32 ws = width_scale(z);
      ds.push_back(hb.Add({-kHB * ws * 0.9f, kDeck - 0.05f, z}, 0.0f, z / 4.0f));
      dp.push_back(hb.Add({kHB * ws * 0.9f, kDeck - 0.05f, z}, 1.0f, z / 4.0f));
    }
    for (u32 si = 0; si < kSt; ++si) hb.Quad(ds[si], ds[si + 1], dp[si + 1], dp[si]);
  }
  {
    const u32 cc = hb.Add({0, (kDeck + kKeel) * 0.5f, -kHL}, 0.5f, 0.5f);
    for (u32 pj = 0; pj < kPr; ++pj) hb.Tri(cc, pj, pj + 1);
  }
  asset::Mesh hull = FinishMesh(hb, "drive/boat_hull", paint.id);
  if (!ctx_.config->headless) ctx_.renderer->UploadMesh(hull);
  boat_mesh_ = hull.id.hash;
}

bool DriveDemo::LoadModel(const std::string& path, i32 skip_mesh_index, Model* out) {
  asset::GltfScene scene;
  if (!asset::LoadGltfScene(path, &scene)) {
    RX_WARN("drive: vendor model '{}' unavailable, using graybox fallback", path.c_str());
    return false;
  }
  if (!ctx_.config->headless) {
    for (const asset::Texture& t : scene.textures)
      if (t.id) ctx_.renderer->UploadTexture(t);
    for (const asset::Material& m : scene.materials) ctx_.renderer->UploadMaterial(m);
    for (const asset::Mesh& m : scene.meshes) ctx_.renderer->UploadMesh(m);
  }
  bool first = true;
  for (const asset::GltfScene::Instance& inst : scene.instances) {
    if (static_cast<i32>(inst.mesh_index) == skip_mesh_index) continue;
    const asset::Mesh& mesh = scene.meshes[inst.mesh_index];
    const Quat q{inst.rotation[0], inst.rotation[1], inst.rotation[2], inst.rotation[3]};
    const Mat4 local = MakeTransform(inst.position, q, inst.scale);
    out->parts.push_back({mesh.id.hash, local});
    // Model-space AABB from the LOD0 vertices (bounds fields may be unset).
    if (!mesh.lods.empty()) {
      for (const asset::Vertex& v : mesh.lods[0].vertices) {
        const Vec3 p = TransformPoint(local, {v.position[0], v.position[1], v.position[2]});
        if (first) {
          out->aabb_min = out->aabb_max = p;
          first = false;
        } else {
          out->aabb_min = {std::min(out->aabb_min.x, p.x), std::min(out->aabb_min.y, p.y),
                           std::min(out->aabb_min.z, p.z)};
          out->aabb_max = {std::max(out->aabb_max.x, p.x), std::max(out->aabb_max.y, p.y),
                           std::max(out->aabb_max.z, p.z)};
        }
      }
    }
  }
  out->loaded = !out->parts.empty();
  return out->loaded;
}

Mat4 DriveDemo::NormalizeXform(const Model& m, f32 target_len, f32 model_yaw,
                               bool sit_on_ground) const {
  const Vec3 c{(m.aabb_min.x + m.aabb_max.x) * 0.5f, (m.aabb_min.y + m.aabb_max.y) * 0.5f,
              (m.aabb_min.z + m.aabb_max.z) * 0.5f};
  const f32 span = std::max(m.aabb_max.x - m.aabb_min.x, m.aabb_max.z - m.aabb_min.z);
  const f32 s = span > 1e-3f ? target_len / span : 1.0f;
  const f32 py = sit_on_ground ? m.aabb_min.y : c.y;
  const Mat4 t = MakeTranslation({-c.x, -py, -c.z});
  const Mat4 scale = MakeScale(s);
  const Mat4 yaw = MakeFromQuat(QuatFromAxisAngle({0, 1, 0}, model_yaw));
  return yaw * scale * t;
}

void DriveDemo::EmitModel(render::FrameView& view, const Model& m, const Mat4& xf) const {
  for (const auto& part : m.parts) {
    render::DrawItem d{};
    d.mesh = part.first;
    d.transform = xf * part.second;
    d.prev_transform = d.transform;
    view.draws.push_back(d);
  }
}

void DriveDemo::SpawnVehicles() {
  physics::PhysicsWorld& phys = *ctx_.physics;

  // Car: VehicleDesc defaults + a trivial torque curve. Spawn a touch high so the
  // suspension settles onto the flattened road.
  physics::PhysicsWorld::VehicleDesc d;
  d.torque_curve[0] = {0.15f, 0.65f};
  d.torque_curve[1] = {0.5f, 1.0f};
  d.torque_curve[2] = {1.0f, 0.82f};
  d.torque_curve_count = 3;
  car_wheel_radius_ = d.wheel_radius;
  car_wheel_width_ = d.wheel_width;
  car_spawn_.y = HeightAt(car_spawn_.x, car_spawn_.z) + 1.0f;
  car_ = phys.CreateVehicle(d, car_spawn_, car_yaw_);

  // Boat on the lake (BoatDesc defaults).
  boat_ = std::make_unique<physics::Boat>(phys, physics::BoatDesc{}, boat_spawn_, boat_yaw_);

  // Plane at the runway threshold; spawn ~0.25 m above rest and let the gear
  // settle. The default 220 kg payload leaves too little excess power (the plane
  // barely climbs and porpoises), so lighten it for a clean demo climb.
  physics::AircraftDesc ad;
  ad.payload_kg = 120.0f;
  plane_spawn_.y = HeightAt(plane_spawn_.x, plane_spawn_.z) + 1.4f;
  aircraft_ = std::make_unique<physics::Aircraft>(phys, ad, plane_spawn_, plane_yaw_);
}

void DriveDemo::SetupAudio() {
  if (!ctx_.audio) return;
  audio::Mixer& mix = ctx_.audio->mixer();
  car_audio_ = std::make_unique<audio::VehicleAudio>(mix, audio::V8Preset());
  boat_audio_ = std::make_unique<audio::VehicleAudio>(mix, audio::InboardBoatPreset());
  plane_audio_ = std::make_unique<audio::VehicleAudio>(mix, audio::SinglePropPlanePreset());
}

f32 DriveDemo::CarMaxSlip() const {
  if (!car_) return 0.0f;
  physics::PhysicsWorld::VehicleState vs;
  if (!ctx_.physics->GetVehicleState(car_, &vs)) return 0.0f;
  f32 slip = 0.0f;
  for (u32 w = 0; w < vs.wheel_count && w < 4; ++w)
    slip = std::max(slip, vs.wheels[w].longitudinal_slip);
  return slip;
}

void DriveDemo::StepVehicles(f32 dt) {
  physics::PhysicsWorld& phys = *ctx_.physics;
  const bool car_on = active_ == Vehicle::kCar;
  const bool boat_on = active_ == Vehicle::kBoat;
  const bool plane_on = active_ == Vehicle::kPlane;

  if (car_) {
    physics::PhysicsWorld::VehicleInput in;
    if (car_on) {
      in.throttle = car_throttle_;
      in.steer = car_steer_;
      in.brake = car_brake_;
      in.handbrake = car_handbrake_;
      in.shift_up = shift_up_pending_;
      in.shift_down = shift_down_pending_;
    } else {
      in.brake = 1.0f;  // inactive car holds still
    }
    phys.DriveVehicle(car_, in);
    shift_up_pending_ = false;
    shift_down_pending_ = false;
  }

  if (boat_ && boat_->valid()) {
    physics::BoatInput bi;
    if (boat_on) {
      bi.throttle = boat_throttle_;
      bi.steer = boat_steer_;
    }
    boat_->Update(bi, dt);
  }

  if (aircraft_ && aircraft_->valid()) {
    physics::AircraftInput ai;
    if (plane_on) {
      ai.throttle = plane_throttle_;
      ai.pitch = plane_pitch_;
      ai.roll = plane_roll_;
      ai.yaw = plane_rudder_;
      ai.flaps = plane_flaps_;
      ai.brakes = plane_brakes_;
    }
    aircraft_->Update(ai, dt);
  }
}

void DriveDemo::ResetActive() {
  const f32 up[4] = {0, 0, 0, 1};
  if (active_ == Vehicle::kCar && car_) {
    ctx_.physics->RemoveVehicle(car_);
    physics::PhysicsWorld::VehicleDesc d;
    d.torque_curve[0] = {0.15f, 0.65f};
    d.torque_curve[1] = {0.5f, 1.0f};
    d.torque_curve[2] = {1.0f, 0.82f};
    d.torque_curve_count = 3;
    car_ = ctx_.physics->CreateVehicle(d, car_spawn_, car_yaw_);
    if (car_) ctx_.physics->SetManualTransmission(car_, car_manual_);
  } else if (active_ == Vehicle::kBoat && boat_ && boat_->valid()) {
    ctx_.physics->SetBodyPosition(boat_->body(), boat_spawn_, up);
  } else if (active_ == Vehicle::kPlane && aircraft_ && aircraft_->valid()) {
    ctx_.physics->SetBodyPosition(aircraft_->body(), plane_spawn_, up);
  }
  cam_init_ = false;
}

void DriveDemo::Update(f32 dt, const InputState& input, const ActionState& actions,
                       bool allow_keyboard, bool allow_mouse) {
  if (dt <= 0) return;
  auto held = [&](Key k) { return allow_keyboard && input.key(k); };

  // Discrete verbs (key edges, render cadence).
  if (allow_keyboard) {
    if (input.key_pressed(Key::kTab)) {
      active_ = static_cast<Vehicle>((static_cast<u32>(active_) + 1) % 3);
      cam_init_ = false;
    }
    if (input.key_pressed(Key::kC)) free_cam_ = !free_cam_;
    if (input.key_pressed(Key::kR)) ResetActive();
    if (input.key_pressed(Key::kM) && car_) {
      car_manual_ = !car_manual_;
      ctx_.physics->SetManualTransmission(car_, car_manual_);
    }
    if (car_manual_) {
      if (input.key_pressed(Key::kLeftShift)) shift_up_pending_ = true;
      if (input.key_pressed(Key::kLeftCtrl)) shift_down_pending_ = true;
    }
    if (input.key_pressed(Key::kF)) {
      flap_step_ = (flap_step_ + 1) % 3;
      plane_flaps_ = static_cast<f32>(flap_step_) * 0.5f;
    }
    // K is absent from the engine Key enum (append-only C# bridge); J drives the
    // rain-wetness cycle instead.
    if (input.key_pressed(Key::kJ)) {
      wetness_step_ = (wetness_step_ + 1) % 3;
      wetness_ = static_cast<f32>(wetness_step_) * 0.5f;
      ctx_.physics->set_surface_wetness(wetness_);
    }
  }

  // Continuous input for the active vehicle; the rest read zero.
  car_throttle_ = car_steer_ = car_brake_ = car_handbrake_ = 0;
  boat_throttle_ = boat_steer_ = 0;
  plane_pitch_ = plane_roll_ = plane_rudder_ = plane_brakes_ = 0;

  if (active_ == Vehicle::kCar) {
    if (held(Key::kW)) car_throttle_ = 1.0f;
    if (held(Key::kS)) {
      const f32 fwd = car_ ? ctx_.physics->VehicleForwardSpeed(car_) : 0.0f;
      if (fwd > 0.6f)
        car_brake_ = 1.0f;  // brake while rolling forward
      else
        car_throttle_ = -1.0f;  // then reverse
    }
    car_steer_ = (held(Key::kD) ? 1.0f : 0.0f) - (held(Key::kA) ? 1.0f : 0.0f);
    if (held(Key::kSpace)) car_handbrake_ = 1.0f;
  } else if (active_ == Vehicle::kBoat) {
    boat_throttle_ = (held(Key::kW) ? 1.0f : 0.0f) - (held(Key::kS) ? 1.0f : 0.0f);
    boat_steer_ = (held(Key::kD) ? 1.0f : 0.0f) - (held(Key::kA) ? 1.0f : 0.0f);
  } else if (active_ == Vehicle::kPlane) {
    if (held(Key::kW)) plane_throttle_ += dt * 0.5f;
    if (held(Key::kS)) plane_throttle_ -= dt * 0.5f;
    plane_throttle_ = std::clamp(plane_throttle_, 0.0f, 1.0f);
    plane_pitch_ = (held(Key::kArrowUp) ? 1.0f : 0.0f) - (held(Key::kArrowDown) ? 1.0f : 0.0f);
    plane_roll_ = (held(Key::kArrowRight) ? 1.0f : 0.0f) - (held(Key::kArrowLeft) ? 1.0f : 0.0f);
    plane_rudder_ = (held(Key::kD) ? 1.0f : 0.0f) - (held(Key::kA) ? 1.0f : 0.0f);
    if (held(Key::kSpace)) plane_brakes_ = 1.0f;
  }

  UpdateChaseCamera(dt, input, actions, allow_keyboard, allow_mouse);

  // Procedural audio: push each vehicle's telemetry every rendered frame.
  if (car_audio_ && car_) {
    physics::PhysicsWorld::VehicleState vs;
    Vec3 p{};
    f32 r[4];
    ctx_.physics->GetVehicleTransform(car_, &p, r);
    audio::VehicleAudioState st;
    if (ctx_.physics->GetVehicleState(car_, &vs)) {
      st.rpm = vs.rpm;
      st.load = vs.engine_load;
      st.speed_mps = std::fabs(vs.forward_speed);
    }
    st.throttle = std::fabs(car_throttle_);
    st.slip = CarMaxSlip();
    st.position = p;
    car_audio_->Update(st);
  }
  if (boat_audio_ && boat_ && boat_->valid()) {
    const physics::BoatState& bs = boat_->state();
    audio::VehicleAudioState st;
    st.rpm = bs.rpm;
    st.load = bs.engine_load;
    st.throttle = std::fabs(bs.throttle);
    st.speed_mps = bs.speed_mps;
    st.submerged = !bs.prop_submerged;
    st.position = bs.position;
    boat_audio_->Update(st);
  }
  if (plane_audio_ && aircraft_ && aircraft_->valid()) {
    const physics::AircraftState& as = aircraft_->state();
    audio::VehicleAudioState st;
    st.rpm = as.rpm;
    st.load = as.engine_load;
    st.throttle = as.throttle;
    st.speed_mps = as.airspeed_mps;
    st.position = as.position;
    plane_audio_->Update(st);
  }
}

void DriveDemo::UpdateChaseCamera(f32 dt, const InputState& input, const ActionState& actions,
                                  bool allow_keyboard, bool allow_mouse) {
  if (free_cam_) {
    ctx_.camera->Update(input, actions, allow_mouse, allow_keyboard, dt);
    cam_eye_ = ctx_.camera->position();
    cam_target_ = ctx_.camera->target();
    cam_init_ = false;  // re-seed the chase smoother when we return
    return;
  }

  Vec3 pos{};
  Quat rot{0, 0, 0, 1};
  f32 dist = 8.0f, height = 3.0f;
  if (active_ == Vehicle::kCar && car_) {
    f32 r[4];
    if (ctx_.physics->GetVehicleTransform(car_, &pos, r)) rot = {r[0], r[1], r[2], r[3]};
    dist = 8.0f;
    height = 3.0f;
  } else if (active_ == Vehicle::kBoat && boat_ && boat_->valid()) {
    pos = boat_->state().position;
    rot = boat_->state().rotation;
    dist = 12.0f;
    height = 5.0f;
  } else if (active_ == Vehicle::kPlane && aircraft_ && aircraft_->valid()) {
    pos = aircraft_->state().position;
    rot = aircraft_->state().rotation;
    dist = 18.0f;
    height = 6.0f;
  }

  const Vec3 fwd = Rotate(rot, {0, 0, 1});
  const Vec3 desired_eye = pos - fwd * dist + Vec3{0, height, 0};
  const Vec3 desired_target = pos + fwd * 2.0f + Vec3{0, 1.0f, 0};
  if (!cam_init_) {
    cam_eye_ = desired_eye;
    cam_target_ = desired_target;
    cam_init_ = true;
  } else {
    const f32 a = 1.0f - std::exp(-dt / 0.14f);
    cam_eye_ = Lerp(cam_eye_, desired_eye, a);
    cam_target_ = Lerp(cam_target_, desired_target, a);
  }
}

void DriveDemo::Emit(f32 dt, render::FrameView& view) {
  (void)dt;
  view.camera.eye = cam_eye_;
  view.camera.target = cam_target_;
  view.camera.fov_y = cam_fov_;

  // Car body + suspension wheels.
  if (car_) {
    Vec3 cp{};
    f32 cr[4];
    if (ctx_.physics->GetVehicleTransform(car_, &cp, cr)) {
      const Mat4 chassis = MakeTransform(cp, {cr[0], cr[1], cr[2], cr[3]}, 1.0f);
      if (car_model_.loaded) {
        EmitModel(view, car_model_, chassis * car_norm_);
      } else {
        render::DrawItem d{};
        d.mesh = graybox_car_;
        d.transform = chassis * ScaleMat({1.8f, 1.0f, 4.0f});
        d.prev_transform = d.transform;
        d.tint = 0xC03828;
        view.draws.push_back(d);
      }
      // Wheels at their live (suspension + steer) transforms. The unit cylinder
      // spins about Y, so rotate it onto the axle (the vehicle's local X).
      const Mat4 axle = MakeFromQuat(QuatFromAxisAngle({0, 0, 1}, kHalfPi));
      const Mat4 wscale = ScaleMat({car_wheel_radius_, car_wheel_width_, car_wheel_radius_});
      for (u32 w = 0; w < 4; ++w) {
        Vec3 wp{};
        f32 wr[4];
        if (!ctx_.physics->GetVehicleWheel(car_, w, &wp, wr)) continue;
        render::DrawItem d{};
        d.mesh = wheel_mesh_;
        d.transform = MakeTransform(wp, {wr[0], wr[1], wr[2], wr[3]}, 1.0f) * axle * wscale;
        d.prev_transform = d.transform;
        view.draws.push_back(d);
      }
    }
  }

  // Boat: procedural hull at the hull pose.
  if (boat_ && boat_->valid()) {
    const Mat4 t = MakeTransform(boat_->state().position, boat_->state().rotation, 1.0f);
    render::DrawItem d{};
    d.mesh = boat_mesh_;
    d.transform = t;
    d.prev_transform = t;
    view.draws.push_back(d);
  }

  // Plane: model framed on the fuselage pose.
  if (aircraft_ && aircraft_->valid()) {
    const Mat4 t = MakeTransform(aircraft_->state().position, aircraft_->state().rotation, 1.0f);
    if (plane_model_.loaded) {
      EmitModel(view, plane_model_, t * plane_norm_);
    } else {
      render::DrawItem d{};
      d.mesh = graybox_plane_;
      d.transform = t * ScaleMat({1.1f, 1.2f, 5.2f});
      d.prev_transform = d.transform;
      d.tint = 0xE8E8E8;
      view.draws.push_back(d);
    }
  }

  // Parked material-showcase pieces.
  for (const auto& piece : showcase_) EmitModel(view, piece.first, piece.second);

  DrawPanel();
}

void DriveDemo::DrawPanel() {
#if defined(RX_HAS_IMGUI)
  if (ImGui::GetCurrentContext() == nullptr) return;
  ImGui::SetNextWindowSize(ImVec2(330, 340), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Drive - vehicle gym")) {
    const char* names[3] = {"CAR", "BOAT", "PLANE"};
    ImGui::Text("active: %s   (Tab to cycle)", names[static_cast<u32>(active_)]);
    ImGui::Text("camera: %s (C)   wetness: %.0f%% (J)", free_cam_ ? "free-fly" : "chase",
                wetness_ * 100.0f);
    ImGui::Separator();

    if (active_ == Vehicle::kCar && car_) {
      physics::PhysicsWorld::VehicleState vs;
      if (ctx_.physics->GetVehicleState(car_, &vs)) {
        const f32 kmh = std::fabs(vs.forward_speed) * 3.6f;
        const char* gear = vs.gear < 0 ? "R" : (vs.gear == 0 ? "N" : nullptr);
        if (gear)
          ImGui::Text("speed %.0f km/h   gear %s   rpm %.0f", kmh, gear, vs.rpm);
        else
          ImGui::Text("speed %.0f km/h   gear %d   rpm %.0f", kmh, vs.gear, vs.rpm);
        ImGui::Text("box: %s   load %.0f%%%s", car_manual_ ? "manual (Shift/Ctrl)" : "automatic",
                    vs.engine_load * 100.0f, vs.is_shifting ? "  (shifting)" : "");
        // Driven wheels are the rear axle (RL=2, RR=3) on the RWD default.
        const physics::SurfaceType surf =
            vs.wheel_count > 2 ? vs.wheels[2].surface : physics::SurfaceType::kAsphalt;
        ImGui::Text("surface: %s   slip %.2f", SurfaceName(surf), CarMaxSlip());
      }
      ImGui::TextDisabled("W throttle  S brake/reverse  A/D steer  Space handbrake");
    } else if (active_ == Vehicle::kBoat && boat_ && boat_->valid()) {
      const physics::BoatState& bs = boat_->state();
      ImGui::Text("speed %.0f km/h   rpm %.0f   load %.0f%%", bs.speed_mps * 3.6f, bs.rpm,
                  bs.engine_load * 100.0f);
      ImGui::Text("planing: %s (%.0f%%)   prop %s", bs.planing > 0.5f ? "YES" : "no",
                  bs.planing * 100.0f, bs.prop_submerged ? "wet" : "ventilated");
      ImGui::TextDisabled("W/S throttle  A/D steer");
    } else if (active_ == Vehicle::kPlane && aircraft_ && aircraft_->valid()) {
      const physics::AircraftState& as = aircraft_->state();
      ImGui::Text("IAS %.0f km/h   alt %.0f m   VS %+.1f m/s", as.airspeed_mps * 3.6f,
                  as.position.y, as.vertical_speed_mps);
      ImGui::Text("throttle %.0f%%   flaps %.0f%%   rpm %.0f", plane_throttle_ * 100.0f,
                  plane_flaps_ * 100.0f, as.rpm);
      ImGui::Text("gear %s", as.on_ground ? "on ground" : "airborne");
      if (as.stalled_left || as.stalled_right) {
        ImGui::TextColored(ImVec4(1, 0.4f, 0.2f, 1), "STALL");
        ImGui::SameLine();
      }
      if (as.over_mtom) ImGui::TextColored(ImVec4(1, 0.7f, 0.2f, 1), "OVERWEIGHT");
      ImGui::TextDisabled("W/S throttle  arrows pitch/roll  A/D rudder  F flaps  Space brakes");
    }
  }
  ImGui::End();
#endif
}

}  // namespace rx
