#include "demo_gym.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "ecs/world.h"
#include "asset/primitives.h"
#include "asset/material.h"
#include "asset/texture.h"
#include "scene/camera.h"
#include "scene/camera_rig.h"
#include "scene/components.h"
#include "inventory/components.h"
#include "inventory/inventory.h"

#if defined(RX_HAS_IMGUI)
#include <imgui.h>
#endif

// The reference gym: procedural dev-checker geometry, a fully wired player
// (capsule locomotion + FP/TP view rig + camera stack), a live tuning panel and
// an inventory drop/pick-up garnish. All sizes are exact metres so a person can
// calibrate eye heights, capsule dims and player scale against known geometry.
namespace rx {
namespace {

constexpr f32 kPi = 3.14159265358979f;

// --- procedural checker textures -------------------------------------------

struct Rgb {
  u8 r, g, b;
};

asset::Texture MakeChecker(asset::AssetId id, u32 res, u32 cells, Rgb a, Rgb b, bool marker,
                           bool grid, Rgb line) {
  asset::Texture tex;
  tex.id = id;
  tex.format = asset::TextureFormat::kRgba8;
  tex.width = res;
  tex.height = res;
  tex.mip_count = 1;
  tex.is_srgb = true;
  tex.data.resize(res * res * 4);
  const u32 cell_px = res / cells;
  const u32 line_w = std::max(1u, res / 128);  // ~2px grid lines
  for (u32 y = 0; y < res; ++y) {
    for (u32 x = 0; x < res; ++x) {
      const u32 cx = x / cell_px;
      const u32 cy = y / cell_px;
      Rgb c = ((cx + cy) & 1u) ? a : b;
      if (grid) {
        const u32 mx = x % cell_px;
        const u32 my = y % cell_px;
        if (mx < line_w || my < line_w || mx >= cell_px - line_w || my >= cell_px - line_w) c = line;
      }
      if (marker && cx == 0 && cy == 0) {
        // Contrasting orientation corner with a small arrow toward +u.
        c = Rgb{0xE0, 0x82, 0x28};
        const f32 fx = static_cast<f32>(x % cell_px) / static_cast<f32>(cell_px);
        const f32 fy = static_cast<f32>(y % cell_px) / static_cast<f32>(cell_px);
        if (fx + std::fabs(fy - 0.5f) < 0.55f && fx > 0.15f) c = Rgb{0x18, 0x18, 0x18};
      }
      u8* p = &tex.data[(y * res + x) * 4];
      p[0] = c.r;
      p[1] = c.g;
      p[2] = c.b;
      p[3] = 0xff;
    }
  }
  return tex;
}

// --- box / geometry accumulation (world space, world-continuous checker UVs) --

struct MeshBuilder {
  asset::Mesh mesh;
  MeshBuilder() { mesh.lods.emplace_back(); }
  asset::MeshLod& lod() { return mesh.lods[0]; }
};

// Six faces as (normal, u-axis, v-axis) with u x v == normal, so the fixed
// corner winding below faces outward.
struct Face {
  Vec3 n, u, v;
};
const Face kFaces[6] = {
    {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},   // +X
    {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},  // -X
    {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},   // +Y
    {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},  // -Y
    {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},   // +Z
    {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},  // -Z
};

void PushVert(MeshBuilder& b, const Vec3& p, const Vec3& n, const Vec3& tan, f32 u, f32 v) {
  asset::Vertex vert{};
  vert.position[0] = p.x;
  vert.position[1] = p.y;
  vert.position[2] = p.z;
  vert.normal[0] = n.x;
  vert.normal[1] = n.y;
  vert.normal[2] = n.z;
  vert.tangent[0] = tan.x;
  vert.tangent[1] = tan.y;
  vert.tangent[2] = tan.z;
  vert.tangent[3] = 1.0f;
  vert.uv[0] = u;
  vert.uv[1] = v;
  vert.color = 0xffffffff;
  b.lod().vertices.push_back(vert);
}

// Axis-aligned box in world space; UVs project world position onto each face's
// (u,v) axes so the checker is continuous across every object at `uv_scale`
// tiles per metre.
void AddBox(MeshBuilder& b, const Vec3& c, const Vec3& h, f32 uv_scale) {
  for (const Face& f : kFaces) {
    const u32 base = static_cast<u32>(b.lod().vertices.size());
    const f32 hu = std::fabs(Dot(f.u, h));
    const f32 hv = std::fabs(Dot(f.v, h));
    const Vec3 fc = c + f.n * std::fabs(Dot(f.n, h));
    const Vec3 corners[4] = {
        fc - f.u * hu - f.v * hv,
        fc + f.u * hu - f.v * hv,
        fc + f.u * hu + f.v * hv,
        fc - f.u * hu + f.v * hv,
    };
    for (const Vec3& p : corners) PushVert(b, p, f.n, f.u, Dot(p, f.u) * uv_scale, Dot(p, f.v) * uv_scale);
    for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) b.lod().indices.push_back(base + i);
  }
}

// Box rotated by `q` about its centre (ramps). UVs come from the local face
// coordinates so the checker lies flat on the tilted surface.
void AddRotatedBox(MeshBuilder& b, const Vec3& c, const Vec3& h, const Quat& q, f32 uv_scale) {
  for (const Face& f : kFaces) {
    const u32 base = static_cast<u32>(b.lod().vertices.size());
    const f32 hu = std::fabs(Dot(f.u, h));
    const f32 hv = std::fabs(Dot(f.v, h));
    const Vec3 fc_local = f.n * std::fabs(Dot(f.n, h));
    const Vec3 local[4] = {
        fc_local - f.u * hu - f.v * hv,
        fc_local + f.u * hu - f.v * hv,
        fc_local + f.u * hu + f.v * hv,
        fc_local - f.u * hu + f.v * hv,
    };
    const Vec3 wn = Rotate(q, f.n);
    const Vec3 wt = Rotate(q, f.u);
    for (const Vec3& lp : local) {
      PushVert(b, c + Rotate(q, lp), wn, wt, Dot(lp, f.u) * uv_scale, Dot(lp, f.v) * uv_scale);
    }
    for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) b.lod().indices.push_back(base + i);
  }
}

u64 UploadBuilder(EngineContext& ctx, MeshBuilder& b, asset::AssetId id, asset::AssetId material) {
  b.mesh.id = id;
  b.lod().submeshes.push_back({0, static_cast<u32>(b.lod().indices.size()), material});
  b.mesh.bounds_center[0] = 0;
  b.mesh.bounds_center[1] = 2;
  b.mesh.bounds_center[2] = 0;
  b.mesh.bounds_radius = 160.0f;  // generous; content spans ~+-25 m
  if (!ctx.config->headless) ctx.renderer->UploadMesh(b.mesh);
  ecs::Entity e = ctx.world->Create();
  ctx.world->Add(e, scene::Transform{});  // geometry already in world space
  ctx.world->Add(e, scene::Renderable{id});
  return id.hash;
}

asset::Material CheckerMaterial(asset::AssetId id, asset::AssetId tex, f32 r, f32 g, f32 bl) {
  asset::Material m;
  m.id = id;
  m.base_color = tex;
  m.base_color_factor[0] = r;
  m.base_color_factor[1] = g;
  m.base_color_factor[2] = bl;
  m.base_color_factor[3] = 1.0f;
  m.roughness_factor = 1.0f;  // fully matte: no sharp sky reflection on the graybox
  m.metallic_factor = 0.0f;
  return m;
}

// Open cylinder around Y, unit radius, unit height (y in [-0.5, 0.5]).
asset::Mesh MakeUnitCylinder(asset::AssetId id, u32 segments) {
  asset::Mesh mesh;
  mesh.id = id;
  mesh.lods.emplace_back();
  auto& lod = mesh.lods[0];
  for (u32 i = 0; i <= segments; ++i) {
    const f32 t = static_cast<f32>(i) / segments;
    const f32 a = t * 2.0f * kPi;
    const Vec3 n{std::cos(a), 0, std::sin(a)};
    for (int k = 0; k < 2; ++k) {
      asset::Vertex v{};
      v.position[0] = n.x;
      v.position[1] = k ? 0.5f : -0.5f;
      v.position[2] = n.z;
      v.normal[0] = n.x;
      v.normal[2] = n.z;
      v.tangent[0] = -n.z;
      v.tangent[2] = n.x;
      v.tangent[3] = 1;
      v.uv[0] = t * 8.0f;
      v.uv[1] = k ? 1.0f : 0.0f;
      lod.vertices.push_back(v);
    }
  }
  for (u32 i = 0; i < segments; ++i) {
    const u32 a = i * 2, b = a + 1, c = a + 2, d = a + 3;
    for (u32 idx : {a, c, b, b, c, d}) lod.indices.push_back(idx);
  }
  mesh.bounds_radius = 1.5f;
  return mesh;
}

Mat4 ScaleMat(const Vec3& s) {
  Mat4 m = Mat4::Identity();
  m.m[0] = s.x;
  m.m[5] = s.y;
  m.m[10] = s.z;
  return m;
}

Quat HeadingQuat(f32 yaw) { return QuatFromAxisAngle({0, -1, 0}, yaw); }

// The character/platform/item update is physics-coupled, so it must advance on
// the same fixed cadence app::Host steps Jolt at, not the variable render frame
// delta (hitches / a varying refresh rate otherwise desync the controller from
// physics). Mirror the Host's fixed step: RX_FIXED_DT when set, else the
// FrameTimer default of 1/60 s.
f32 GymFixedStep() {
  if (const char* env = std::getenv("RX_FIXED_DT")) {
    const f32 v = std::strtof(env, nullptr);
    if (v > 0.0f) return v;
  }
  return 1.0f / 60.0f;
}

}  // namespace

GymDemo::GymDemo(EngineContext& ctx) : ctx_(ctx) {}

void GymDemo::Create() {
  // --- lighting: simple, bright, no lens-flare artifacts ---------------------
  if (!ctx_.config->headless) {
    auto& s = ctx_.renderer->settings();
    s.sun_direction = Normalize(Vec3{-0.4f, -0.85f, -0.35f});
    s.sun_intensity = 4.2f;
    s.sun_color = {1.0f, 0.97f, 0.92f};
    s.ambient = 0.26f;
    s.night = 0.0f;
    s.lens_flare = 0.0f;      // graybox: no sun-flare streaks over the checker
    s.rt_reflections = false;  // flat, readable surfaces (no sky mirrored in the checker)
    s.ssr = false;
  }
  ctx_.scene_owns_sun = true;  // stop the day/night clock re-driving the sun

  BuildContent();
  BuildPlayer();

  // Env-gated staging for captures. RX_GYM_VIEW=fp|tp, RX_GYM_SPAWN="x,z"[,y],
  // RX_GYM_YAW=<radians>, RX_GYM_SCRIPT="t:token,t:token,...".
  if (const char* spawn = std::getenv("RX_GYM_SPAWN")) {
    Vec3 p = spawn_feet_;
    if (std::sscanf(spawn, "%f,%f,%f", &p.x, &p.z, &p.y) >= 2) {
      spawn_feet_ = p;
      ResetPlayer();
    }
  }
  if (const char* yaw = std::getenv("RX_GYM_YAW")) {
    spawn_yaw_ = std::strtof(yaw, nullptr);
    if (auto* st = ctx_.world->Get<character::CharacterState>(player_)) st->yaw = spawn_yaw_;
  }
  if (const char* pitch = std::getenv("RX_GYM_PITCH")) {
    if (auto* orbit = ctx_.world->Get<scene::CameraOrbit>(player_))
      orbit->pitch = std::strtof(pitch, nullptr);
  }
  if (const char* view = std::getenv("RX_GYM_VIEW")) {
    const bool want_tp = std::strcmp(view, "tp") == 0;
    auto* vm = ctx_.world->Get<character::CharacterViewMode>(player_);
    if (vm && ((vm->kind == character::CharacterViewKind::kThirdPerson) != want_tp)) {
      character::ToggleCharacterViewMode(*ctx_.world, player_, camera_output_, player_,
                                         view_settings_, {.duration = 0.0f});
    }
  }
  if (const char* script = std::getenv("RX_GYM_SCRIPT")) {
    std::string s(script);
    size_t pos = 0;
    while (pos < s.size()) {
      size_t comma = s.find(',', pos);
      if (comma == std::string::npos) comma = s.size();
      std::string entry = s.substr(pos, comma - pos);
      pos = comma + 1;
      size_t colon = entry.find(':');
      if (colon == std::string::npos) continue;
      ScriptStep step;
      step.time = std::strtof(entry.substr(0, colon).c_str(), nullptr);
      step.token = entry.substr(colon + 1);
      script_.push_back(step);
    }
    if (!script_.empty()) {
      mouse_captured_ = false;  // scripted run: don't grab the cursor
      if (ctx_.debug_ui) ctx_.debug_ui->SetVisible(false);  // clean capture; keep only the gym panel
    }
    RX_INFO("gym: RX_GYM_SCRIPT with {} steps", script_.size());
  }

  RX_INFO(
      "gym demo: WASD move, mouse look, Shift sprint, Ctrl crouch, Space jump, V toggle FP/TP, "
      "scroll zoom, G drop, F pick up, Tab release cursor");
}

void GymDemo::BuildContent() {
  // Mid-gray two-tone (classic dev-checker); kept clear of near-white so the
  // engine's RT sky reflections do not read as translucency on the flat faces.
  asset::Texture cube_tex =
      MakeChecker(asset::MakeAssetId("gym/checker"), 256, 8, {0x8c, 0x8e, 0x92}, {0x4a, 0x4c, 0x50},
                  /*marker=*/true, /*grid=*/false, {});
  asset::Texture grid_tex =
      MakeChecker(asset::MakeAssetId("gym/grid"), 256, 2, {0x8c, 0x92, 0x98}, {0x6a, 0x70, 0x76},
                  /*marker=*/false, /*grid=*/true, {0x22, 0x24, 0x27});
  if (!ctx_.config->headless) {
    ctx_.renderer->UploadTexture(cube_tex);
    ctx_.renderer->UploadTexture(grid_tex);
  }

  asset::Material ground_mat =
      CheckerMaterial(asset::MakeAssetId("gym/ground_mat"), grid_tex.id, 1.0f, 1.0f, 1.0f);
  asset::Material cube_mat =
      CheckerMaterial(asset::MakeAssetId("gym/cube_mat"), cube_tex.id, 1.0f, 1.0f, 1.0f);
  asset::Material structure_mat =
      CheckerMaterial(asset::MakeAssetId("gym/structure_mat"), cube_tex.id, 0.72f, 0.82f, 1.0f);
  asset::Material furniture_mat =
      CheckerMaterial(asset::MakeAssetId("gym/furniture_mat"), cube_tex.id, 1.0f, 0.78f, 0.5f);
  asset::Material player_mat =
      CheckerMaterial(asset::MakeAssetId("gym/player_mat"), cube_tex.id, 1.0f, 1.0f, 1.0f);
  asset::Material platform_mat =
      CheckerMaterial(asset::MakeAssetId("gym/platform_mat"), cube_tex.id, 0.35f, 1.0f, 0.45f);
  if (!ctx_.config->headless) {
    ctx_.renderer->UploadMaterial(ground_mat);
    ctx_.renderer->UploadMaterial(cube_mat);
    ctx_.renderer->UploadMaterial(structure_mat);
    ctx_.renderer->UploadMaterial(furniture_mat);
    ctx_.renderer->UploadMaterial(player_mat);
    ctx_.renderer->UploadMaterial(platform_mat);
  }
  physics::PhysicsWorld& phys = *ctx_.physics;

  // Ground: 60x60 m checker plane, top surface at y = 0, 1 m grid cells.
  {
    MeshBuilder g;
    AddBox(g, {0, -0.5f, 0}, {30, 0.5f, 30}, 0.5f);  // grid tile = 2 m -> 1 m cells
    UploadBuilder(ctx_, g, asset::MakeAssetId("gym/ground"), ground_mat.id);
    phys.AddStaticBox({0, -0.5f, 0}, {30, 0.5f, 30});
  }

  auto solid_box = [&](MeshBuilder& b, const Vec3& c, const Vec3& h) {
    AddBox(b, c, h, 1.0f);
    phys.AddStaticBox(c, h);
  };

  // --- reference cubes: 0.25 / 0.5 / 1.0 / 2.0 m, resting on the ground -------
  MeshBuilder cubes;
  const f32 sizes[4] = {0.25f, 0.5f, 1.0f, 2.0f};
  f32 cx = -3.0f;
  for (f32 sz : sizes) {
    const f32 hh = sz * 0.5f;
    solid_box(cubes, {cx, hh, 4.0f}, {hh, hh, hh});
    cx += sz + 1.0f;
  }
  UploadBuilder(ctx_, cubes, asset::MakeAssetId("gym/cubes"), cube_mat.id);

  // --- furniture-scale obstacles: 0.75 m table, 0.45 m seat, 1.0 m counter ----
  MeshBuilder furn;
  solid_box(furn, {-2.0f, 0.375f, 6.5f}, {0.6f, 0.375f, 0.4f});  // table top at 0.75 m
  solid_box(furn, {-3.2f, 0.225f, 6.5f}, {0.22f, 0.225f, 0.22f});  // seat top at 0.45 m
  solid_box(furn, {1.6f, 0.5f, 6.5f}, {0.9f, 0.5f, 0.3f});          // counter top at 1.0 m
  UploadBuilder(ctx_, furn, asset::MakeAssetId("gym/furniture"), furniture_mat.id);

  // --- structure: doorway, stairs, ramps, crouch tunnel, narrow gap ----------
  MeshBuilder st;

  // Doorway frame: 1.0 m clear width, 2.1 m clear height.
  {
    const f32 x = 4.5f, z = 4.0f;
    const f32 clear_w = 1.0f, clear_h = 2.1f, jamb = 0.2f, depth = 0.25f;
    solid_box(st, {x - clear_w * 0.5f - jamb * 0.5f, clear_h * 0.5f, z},
              {jamb * 0.5f, clear_h * 0.5f, depth});  // left jamb
    solid_box(st, {x + clear_w * 0.5f + jamb * 0.5f, clear_h * 0.5f, z},
              {jamb * 0.5f, clear_h * 0.5f, depth});  // right jamb
    solid_box(st, {x, clear_h + 0.15f, z}, {clear_w * 0.5f + jamb, 0.15f, depth});  // lintel
  }

  // Staircases: eight steps each, 0.15 m and 0.30 m risers, 0.3 m going.
  auto stairs = [&](f32 x0, f32 z0, f32 riser) {
    const f32 going = 0.3f, width = 1.4f;
    for (int i = 0; i < 8; ++i) {
      const f32 top = riser * (i + 1);
      const f32 z = z0 - going * i;
      solid_box(st, {x0, top * 0.5f, z - going * 0.5f}, {width * 0.5f, top * 0.5f, going * 0.5f});
    }
  };
  stairs(-6.0f, -1.0f, 0.15f);
  stairs(-3.0f, -1.0f, 0.30f);

  // Ramps at 30 / 45 / 60 deg (the 60 deg one is above the ~55 deg slope limit,
  // so it is deliberately unwalkable). Each is a tilted slab 3 m along the run.
  auto ramp = [&](f32 x, f32 z, f32 deg) {
    const f32 ang = deg * kPi / 180.0f;
    const f32 run = 3.0f, w = 1.4f, thick = 0.15f;
    const Quat q = QuatFromAxisAngle({1, 0, 0}, -ang);  // tilt up toward -Z
    const Vec3 center{x, std::sin(ang) * run * 0.5f + 0.05f, z - std::cos(ang) * run * 0.5f};
    AddRotatedBox(st, center, {w * 0.5f, thick * 0.5f, run * 0.5f}, q, 1.0f);
    physics::ShapeDesc box;
    box.kind = physics::ShapeDesc::Kind::kBox;
    box.half_extents = {w * 0.5f, thick * 0.5f, run * 0.5f};
    const f32 rot[4] = {q.x, q.y, q.z, q.w};
    phys.AddStaticShape(box, center, rot, 1.0f);
  };
  ramp(3.5f, 3.0f, 30.0f);
  ramp(5.5f, 3.0f, 45.0f);
  ramp(7.5f, 3.0f, 60.0f);

  // Low crouch tunnel: 1.25 m clearance (forces crouch; the 1.8 m standing
  // capsule cannot uncrouch under the roof).
  {
    const f32 x = -8.0f, z = 4.0f, len = 4.0f, clear = 1.25f, wall = 0.3f, width = 1.4f;
    solid_box(st, {x - width * 0.5f - wall * 0.5f, clear * 0.5f, z},
              {wall * 0.5f, clear * 0.5f, len * 0.5f});
    solid_box(st, {x + width * 0.5f + wall * 0.5f, clear * 0.5f, z},
              {wall * 0.5f, clear * 0.5f, len * 0.5f});
    solid_box(st, {x, clear + 0.15f, z}, {width * 0.5f + wall, 0.15f, len * 0.5f});  // roof
  }

  // Narrow 0.7 m gap between two walls (squeeze test).
  {
    const f32 x = 0.0f, z = -3.0f, gap = 0.7f, wall_len = 2.5f, wall_h = 1.6f, wall_t = 0.3f;
    solid_box(st, {x - gap * 0.5f - wall_len * 0.5f, wall_h * 0.5f, z},
              {wall_len * 0.5f, wall_h * 0.5f, wall_t * 0.5f});
    solid_box(st, {x + gap * 0.5f + wall_len * 0.5f, wall_h * 0.5f, z},
              {wall_len * 0.5f, wall_h * 0.5f, wall_t * 0.5f});
  }

  UploadBuilder(ctx_, st, asset::MakeAssetId("gym/structure"), structure_mat.id);

  // --- kinematic moving platform (demonstration only) ------------------------
  platform_center_ = {9.0f, 0.25f, 8.0f};
  platform_span_ = 3.0f;
  {
    MeshBuilder pf;
    AddBox(pf, {0, 0, 0}, {0.9f, 0.15f, 0.9f}, 1.0f);  // local; entity transform moves it
    pf.mesh.id = asset::MakeAssetId("gym/platform");
    pf.lod().submeshes.push_back({0, static_cast<u32>(pf.lod().indices.size()), platform_mat.id});
    pf.mesh.bounds_radius = 2.0f;
    if (!ctx_.config->headless) ctx_.renderer->UploadMesh(pf.mesh);
    platform_mesh_ = pf.mesh.id.hash;
    platform_ = ctx_.world->Create();
    ctx_.world->Add(platform_, scene::Transform{.position = {platform_center_.x, platform_center_.y,
                                                             platform_center_.z}});
    ctx_.world->Add(platform_, scene::Renderable{pf.mesh.id});
    platform_body_ = phys.AddKinematicBox(platform_center_, {0.9f, 0.15f, 0.9f});
  }

  // Player proxy pieces (unit cylinder body + unit sphere caps; scaled per frame).
  {
    asset::Mesh cyl = MakeUnitCylinder(asset::MakeAssetId("gym/player_body"), 24);
    cyl.lods[0].submeshes.push_back(
        {0, static_cast<u32>(cyl.lods[0].indices.size()), player_mat.id});
    asset::Mesh cap = asset::MakeSphere(1.0f, 10, 16, asset::MakeAssetId("gym/player_cap"));
    for (auto& sm : cap.lods[0].submeshes) sm.material = player_mat.id;
    if (cap.lods[0].submeshes.empty())
      cap.lods[0].submeshes.push_back(
          {0, static_cast<u32>(cap.lods[0].indices.size()), player_mat.id});
    if (!ctx_.config->headless) {
      ctx_.renderer->UploadMesh(cyl);
      ctx_.renderer->UploadMesh(cap);
    }
    capsule_body_mesh_ = cyl.id.hash;
    capsule_cap_mesh_ = cap.id.hash;
  }

  // --- inventory: one "crate" item def (0.25 m checker cube) ------------------
  {
    MeshBuilder crate;
    AddBox(crate, {0, 0, 0}, {0.125f, 0.125f, 0.125f}, 1.0f);
    crate.mesh.id = asset::MakeAssetId("gym/crate");
    crate.lod().submeshes.push_back(
        {0, static_cast<u32>(crate.lod().indices.size()), platform_mat.id});
    crate.mesh.bounds_radius = 0.5f;
    if (!ctx_.config->headless) ctx_.renderer->UploadMesh(crate.mesh);
    crate_mesh_ = crate.mesh.id.hash;

    inventory::ItemDef def;
    def.name_hash = asset::MakeAssetId("gym.crate").hash;
    def.world_mesh = crate.mesh.id;
    def.shape.kind = physics::ShapeDesc::Kind::kBox;
    def.shape.half_extents = {0.125f, 0.125f, 0.125f};
    def.mass = 6.0f;
    def.friction = 0.6f;
    def.restitution = 0.1f;
    def.scale = 1.0f;
    def.max_stack = 99;
    def.weight = 2.0f;
    crate_def_ = catalog_.Register(def);
  }
}

void GymDemo::BuildPlayer() {
  ecs::World& world = *ctx_.world;
  physics::PhysicsWorld& phys = *ctx_.physics;

  character::CharacterShape shape;   // engine defaults (0.3r x 1.8h, eye 1.62 m)
  character::CharacterMovementSettings move;
  const f32 total_half = character::CharacterShape{}.standing_height * 0.5f;  // 0.9 to capsule tip
  const f32 radius = shape.standing_radius;
  const f32 half_height = std::max(shape.standing_height * 0.5f - radius, 0.01f);

  player_ = world.Create();
  world.Add(player_, scene::Transform{.position = {spawn_feet_.x, spawn_feet_.y, spawn_feet_.z}});
  world.Add(player_, shape);
  world.Add(player_, move);
  world.Add(player_, character::CharacterIntent{});
  world.Add(player_, character::CharacterState{});
  world.Add(player_, character::CharacterViewMode{});  // starts first person

  const Vec3 center = spawn_feet_ + Vec3{0, total_half, 0};
  physics::CharacterId cid = phys.CreateCharacter(center, radius, half_height);
  world.Add(player_, character::CharacterBody{cid, radius, half_height, false});

  // Jetpack (off until toggled with J): engine-default desc + its input/telemetry
  // components, plus a LightJetPreset voice positioned at the player.
  world.Add(player_, character::JetpackDesc{});
  world.Add(player_, character::JetpackInput{});
  world.Add(player_, character::JetpackState{});
  if (ctx_.audio)
    jetpack_audio_ = std::make_unique<audio::VehicleAudio>(ctx_.audio->mixer(),
                                                           audio::LightJetPreset());

  // Inventory with a handful of crates to drop.
  inventory::Inventory inv;
  inv.max_entries = 16;
  world.Add(player_, inv);
  if (auto* pinv = world.Get<inventory::Inventory>(player_))
    inventory::AddItem(*pinv, catalog_, crate_def_, 8);

  // Camera rig recipe (installs anchor/intent/orbit + FP/TP components) and a
  // camera-stack output entity whose CameraOutput we read back each frame.
  character::ApplyCharacterViewMode(world, player_, view_settings_);
  camera_output_ = world.Create();
  scene::InitializeCameraStack(world, camera_output_, player_);

  if (auto* st = world.Get<character::CharacterState>(player_)) st->yaw = spawn_yaw_;
}

void GymDemo::ResetPlayer() {
  spawn_feet_.y = std::max(spawn_feet_.y, 0.0f);
  character::TeleportCharacter(*ctx_.world, *ctx_.physics, player_, spawn_feet_);
  if (auto* st = ctx_.world->Get<character::CharacterState>(player_)) st->yaw = spawn_yaw_;
}

void GymDemo::FillIntent(const InputState& input, const ActionState& actions, bool allow_keyboard,
                         bool allow_mouse, f32 dt) {
  auto* intent = ctx_.world->Get<character::CharacterIntent>(player_);
  auto* state = ctx_.world->Get<character::CharacterState>(player_);
  if (!intent || !state) return;

  f32 fwd = 0, right = 0;
  bool sprint = false, crouch = false, jump = false;
  f32 yaw_delta = 0, pitch_delta = 0;

  if (!script_.empty()) {
    RunScript(dt);
    fwd = script_move_fwd_;
    right = script_move_right_;
    sprint = script_sprint_;
    crouch = script_crouch_;
  } else {
    if (allow_keyboard) {
      fwd = -actions.axis(Axis::kMoveY);  // W = forward (SDL stick-down is +)
      right = actions.axis(Axis::kMoveX);
      sprint = actions.down(Action::kSprint);
      crouch = actions.down(Action::kSneak);
      jump = actions.pressed(Action::kJump);
    }
    if (mouse_captured_ && allow_mouse) {
      yaw_delta = input.mouse_dx * look_sensitivity_;
      pitch_delta = -input.mouse_dy * look_sensitivity_ * (invert_pitch_ ? -1.0f : 1.0f);
      // Gamepad right stick (rate based).
      yaw_delta += actions.axis(Axis::kLookX) * 2.4f * dt;
      pitch_delta -= actions.axis(Axis::kLookY) * 2.4f * dt * (invert_pitch_ ? -1.0f : 1.0f);
    }
  }

  // World-space move from the heading (WASD is camera-relative because the
  // camera yaw follows the heading).
  const Quat heading = HeadingQuat(state->yaw);
  const Vec3 f = Rotate(heading, {0, 0, -1});
  const Vec3 r = Rotate(heading, {1, 0, 0});
  Vec3 move = f * fwd + r * right;
  const f32 len = Length(move);
  if (len > 1.0f) move = move * (1.0f / len);
  intent->move = move;
  intent->gait = sprint ? character::CharacterGait::kSprint : character::CharacterGait::kRun;
  intent->crouch = crouch;
  if (jump) intent->jump = true;
  // Accumulate look (and jump, above) rather than overwrite: FillIntent runs once
  // per render frame but StepCharacters runs on the fixed accumulator, so on a
  // frame that steps zero times this frame's mouse motion must survive to the
  // next step instead of being clobbered. StepCharacters zeroes both on consume.
  intent->look_yaw_delta += yaw_delta;
  intent->look_pitch_delta += pitch_delta;
}

void GymDemo::RunScript(f32 dt) {
  script_time_ += dt;
  while (script_cursor_ < script_.size() && script_[script_cursor_].time <= script_time_) {
    const std::string& t = script_[script_cursor_].token;
    ++script_cursor_;
    if (t == "fwd") { script_move_fwd_ = 1; script_move_right_ = 0; }
    else if (t == "fwdhalf") { script_move_fwd_ = 0.5f; script_move_right_ = 0; }  // analog: half stick
    else if (t == "back") { script_move_fwd_ = -1; script_move_right_ = 0; }
    else if (t == "left") { script_move_fwd_ = 0; script_move_right_ = -1; }
    else if (t == "right") { script_move_fwd_ = 0; script_move_right_ = 1; }
    else if (t == "stop") { script_move_fwd_ = 0; script_move_right_ = 0; }
    else if (t == "sprint") script_sprint_ = true;
    else if (t == "run") script_sprint_ = false;
    else if (t == "crouch") script_crouch_ = true;
    else if (t == "stand") script_crouch_ = false;
    else if (t == "jump") { if (auto* i = ctx_.world->Get<character::CharacterIntent>(player_)) i->jump = true; }
    else if (t == "view") character::ToggleCharacterViewMode(*ctx_.world, player_, camera_output_, player_, view_settings_, {.duration = 0.25f});
    else if (t == "drop") DropCrate();
    else if (t == "pickup") PickUpNearest();
  }
}

void GymDemo::SyncViewSettingsToRig() {
  ecs::World& world = *ctx_.world;
  const character::CharacterViewSettings& vs = view_settings_;
  if (auto* boom = world.Get<scene::CameraBoom>(player_)) {
    boom->shoulder_offset = vs.tp_shoulder_offset;
    boom->height_offset = vs.tp_height_offset;
    boom->distance = std::clamp(boom->distance, vs.tp_min_distance, vs.tp_max_distance);
  }
  if (auto* ob = world.Get<scene::CameraObstruction>(player_)) {
    ob->radius = vs.tp_obstruction_radius;
    ob->margin = vs.tp_obstruction_margin;
  }
  if (auto* dmp = world.Get<scene::CameraDamping>(player_)) {
    dmp->position_half_life = vs.tp_position_half_life;
    dmp->rotation_half_life = vs.tp_rotation_half_life;
  }
  auto* vm = world.Get<character::CharacterViewMode>(player_);
  if (auto* orbit = world.Get<scene::CameraOrbit>(player_)) {
    if (vm && vm->kind == character::CharacterViewKind::kFirstPerson) {
      orbit->min_pitch = -vs.fp_pitch_limit;
      orbit->max_pitch = vs.fp_pitch_limit;
    } else {
      orbit->min_pitch = vs.tp_pitch_min;
      orbit->max_pitch = vs.tp_pitch_max;
    }
  }
}

void GymDemo::DropCrate() {
  auto* inv = ctx_.world->Get<inventory::Inventory>(player_);
  auto* st = ctx_.world->Get<character::CharacterState>(player_);
  auto* tr = ctx_.world->Get<scene::Transform>(player_);
  if (!inv || !st || !tr) return;
  // Find the crate's inventory entry.
  u32 entry = 0;
  bool found = false;
  for (u32 i = 0; i < inv->entries.size(); ++i) {
    if (inv->entries[i].item == crate_def_ && inv->entries[i].count > 0) {
      entry = i;
      found = true;
      break;
    }
  }
  if (!found) return;
  const Quat heading = HeadingQuat(st->yaw);
  const Vec3 f = Rotate(heading, {0, 0, -1});
  const Vec3 feet{tr->position[0], tr->position[1], tr->position[2]};
  scene::Transform spawn;
  const Vec3 p = feet + Vec3{0, st->eye_height * 0.7f, 0} + f * 0.6f;
  spawn.position[0] = p.x;
  spawn.position[1] = p.y;
  spawn.position[2] = p.z;
  const Vec3 impulse = (f + Vec3{0, 0.5f, 0}) * 9.0f;  // gentle forward lob, lands just ahead
  inventory::DropItem(*ctx_.world, *ctx_.physics, catalog_, player_, entry, 1, spawn, impulse);
}

void GymDemo::PickUpNearest() {
  auto* tr = ctx_.world->Get<scene::Transform>(player_);
  if (!tr) return;
  const Vec3 feet{tr->position[0], tr->position[1], tr->position[2]};
  ecs::Entity best{};
  f32 best_d2 = 2.5f * 2.5f;  // pick-up reach
  ctx_.world->Each<inventory::WorldItem, scene::Transform>(
      [&](ecs::Entity e, inventory::WorldItem&, scene::Transform& t) {
        const Vec3 d{t.position[0] - feet.x, t.position[1] - feet.y, t.position[2] - feet.z};
        const f32 d2 = Dot(d, d);
        if (d2 < best_d2) {
          best_d2 = d2;
          best = e;
        }
      });
  if (best) inventory::PickUpItem(*ctx_.world, *ctx_.physics, catalog_, best, player_);
}

void GymDemo::Update(f32 dt, const InputState& input, const ActionState& actions,
                     bool allow_keyboard, bool allow_mouse) {
  if (!player_ || dt <= 0) return;
  ecs::World& world = *ctx_.world;
  physics::PhysicsWorld& phys = *ctx_.physics;

  // Cursor release toggle (Tab), so the tuning panel is clickable.
  if (allow_keyboard && input.key_pressed(Key::kTab)) mouse_captured_ = !mouse_captured_;

  // Discrete verbs (raw keys; edges from InputState) stay on render cadence: one
  // edge per input pump.
  if (script_.empty() && mouse_captured_ && allow_keyboard) {
    if (input.key_pressed(Key::kV))
      character::ToggleCharacterViewMode(world, player_, camera_output_, player_, view_settings_,
                                         {.duration = 0.25f});
    if (input.key_pressed(Key::kG)) DropCrate();
    if (input.key_pressed(Key::kF) || input.key_pressed(Key::kT)) PickUpNearest();
    if (input.key_pressed(Key::kR)) ResetPlayer();
    if (input.key_pressed(Key::kJ)) {  // toggle the jetpack on / off
      if (auto* ji = world.Get<character::JetpackInput>(player_)) ji->enabled = !ji->enabled;
    }
    if (input.wheel != 0.0f)
      character::ApplyCharacterZoom(world, player_, input.wheel * 0.4f, false, view_settings_);
  }

  const f32 fixed = GymFixedStep();
  // Gather this render frame's input once. Scripted staging advances exactly one
  // fixed step per render frame so a capture at frame N stays frame-index
  // deterministic regardless of the windowed frame rate.
  FillIntent(input, actions, allow_keyboard, allow_mouse, script_.empty() ? dt : fixed);
  SyncViewSettingsToRig();

  // Jetpack control: with the pack ON, holding the jump input (Space) thrusts and
  // the ordinary jump is suppressed; with it OFF, Space stays the normal jump.
  if (auto* jin = world.Get<character::JetpackInput>(player_)) {
    if (jin->enabled) {
      jin->thrust = allow_keyboard && actions.down(Action::kJump);
      if (auto* ci = world.Get<character::CharacterIntent>(player_)) ci->jump = false;
    } else {
      jin->thrust = false;
    }
  }

  int steps;
  if (!script_.empty()) {
    steps = 1;
  } else {
    // Mirror app::Host's fixed-step accumulator (same 0.25 s clamp against a
    // spiral of death) so the physics-coupled update advances on the engine's
    // Jolt cadence rather than the raw render frame delta.
    static f32 sim_accum = 0.0f;
    sim_accum += std::min(dt, 0.25f);
    steps = 0;
    while (sim_accum >= fixed) {
      sim_accum -= fixed;
      ++steps;
    }
  }

  for (int i = 0; i < steps; ++i) {
    // Jetpack BEFORE the character step: it writes CharacterIntent's external
    // acceleration, which StepCharacters then folds into the velocity.
    character::StepJetpacks(world, fixed);
    // Character + camera-rig pipeline (README staged order).
    character::StepCharacters(world, phys, fixed);
    character::SyncCharacterCameraAnchors(world);
    scene::BuildCameraRigs(world, fixed);
    scene::PrepareCameraRigConstraints(world, fixed);
    character::AnswerCameraObstructions(world, phys);
    scene::ResolveCameraRigs(world, fixed);
    scene::ResolveCameraStacks(world, fixed);

    // Moving platform: ping-pong along X, driven kinematically so a standing body
    // could ride it (character platform-riding is not yet folded into the module).
    platform_time_ += fixed;
    const f32 offset = std::sin(platform_time_ * 0.6f) * platform_span_;
    const Vec3 target{platform_center_.x + offset, platform_center_.y, platform_center_.z};
    const f32 identity_rot[4] = {0, 0, 0, 1};
    if (platform_body_) phys.MoveBodyKinematic(platform_body_, target, identity_rot, fixed);

    // Dropped-item lifecycle (README per-tick order).
    Vec3 player_pos = spawn_feet_;
    if (auto* tr = world.Get<scene::Transform>(player_))
      player_pos = {tr->position[0], tr->position[1], tr->position[2]};
    inventory::SyncWorldItems(world, phys);
    inventory::HibernateDistantWorldItems(world, phys, store_, player_pos, 40.0f);
    inventory::WakeWorldItemsNear(world, phys, catalog_, store_, player_pos, 30.0f);
  }

  // Read the resolved platform transform + camera back for this render frame
  // (last simulated state; purely visual, so render cadence is fine).
  if (auto* pt = world.Get<scene::Transform>(platform_)) {
    Vec3 pos;
    f32 rot[4];
    if (phys.GetBodyTransform(platform_body_, &pos, rot)) {
      pt->position[0] = pos.x;
      pt->position[1] = pos.y;
      pt->position[2] = pos.z;
    }
  }
  if (auto* out = world.Get<scene::CameraOutput>(camera_output_)) {
    const scene::CameraView& v = out->view;
    cam_eye_ = v.position;
    cam_target_ = v.position + Rotate(v.orientation, Vec3{0, 0, -1});
    cam_fov_ = v.lens.fov_y;
    cam_valid_ = out->valid;
  }

  // Jetpack voice: N1 (rpm) tracks the spooled thrust (turbine whine), the roar
  // tracks the burn demand, and an empty tank ducks/muffles it (the sputter-out).
  if (jetpack_audio_) {
    auto* jst = world.Get<character::JetpackState>(player_);
    auto* jin = world.Get<character::JetpackInput>(player_);
    audio::VehicleAudioState st;
    if (jst) {
      st.rpm = jst->thrust * 100.0f;  // N1 %
      st.load = jst->thrust;
      st.thrust = (jin && jin->enabled && jin->thrust) ? 1.0f : 0.0f;  // roar <- demand
      st.throttle = st.thrust;
      st.submerged = jst->fuel <= 0.0f;  // dry tank: duck + muffle
    }
    if (auto* tr = world.Get<scene::Transform>(player_))
      st.position = {tr->position[0], tr->position[1], tr->position[2]};
    jetpack_audio_->Update(st);
  }
}

void GymDemo::Emit(f32 dt, render::FrameView& view) {
  (void)dt;
  if (cam_valid_) {
    view.camera.eye = cam_eye_;
    view.camera.target = cam_target_;
    view.camera.fov_y = cam_fov_;
  }

  // Player proxy capsule (cylinder body + two sphere caps), hidden in first
  // person. Sized from the live capsule so it shrinks when crouched.
  auto* vm = ctx_.world->Get<character::CharacterViewMode>(player_);
  auto* body = ctx_.world->Get<character::CharacterBody>(player_);
  auto* tr = ctx_.world->Get<scene::Transform>(player_);
  auto* st = ctx_.world->Get<character::CharacterState>(player_);
  const bool third_person = vm && vm->kind == character::CharacterViewKind::kThirdPerson;
  if (third_person && body && tr && st) {
    const f32 radius = body->radius;
    const f32 cyl_len = std::max(2.0f * body->half_height, 0.01f);
    const Vec3 feet{tr->position[0], tr->position[1], tr->position[2]};
    const f32 center_y = feet.y + radius + cyl_len * 0.5f;
    // Body faces the smoothed facing yaw (turn smoothing) — in third person it
    // eases toward the movement direction rather than snapping to the look yaw.
    const Mat4 rot = MakeFromQuat(HeadingQuat(st->facing_yaw));
    const u32 tint = 0x4a90d0;  // cool blue so it reads against the gray checker

    Mat4 body_m =
        MakeTranslation({feet.x, center_y, feet.z}) * rot * ScaleMat({radius, cyl_len, radius});
    render::DrawItem d{};
    d.mesh = capsule_body_mesh_;
    d.transform = body_m;
    d.prev_transform = body_m;
    d.tint = tint;
    view.draws.push_back(d);

    for (int s = -1; s <= 1; s += 2) {
      const f32 cy = center_y + s * cyl_len * 0.5f;
      Mat4 cap_m = MakeTranslation({feet.x, cy, feet.z}) * rot * MakeScale(radius);
      render::DrawItem c{};
      c.mesh = capsule_cap_mesh_;
      c.transform = cap_m;
      c.prev_transform = cap_m;
      c.tint = tint;
      view.draws.push_back(c);
    }
  }

  DrawPanel();
}

void GymDemo::DrawPanel() {
#if defined(RX_HAS_IMGUI)
  // The compile guard only proves imgui is available, not that a live context /
  // frame exists. The Viewer keeps running when DebugUi init fails (renderer stub
  // or UI-init failure) and only starts the imgui frame while initialized, so
  // with no context ImGui::Begin below would assert — bail at runtime too. (Same
  // GetCurrentContext() guard DebugUi::wants_mouse/keyboard use.)
  if (ImGui::GetCurrentContext() == nullptr) return;
  ecs::World& world = *ctx_.world;
  auto* shape = world.Get<character::CharacterShape>(player_);
  auto* move = world.Get<character::CharacterMovementSettings>(player_);
  auto* state = world.Get<character::CharacterState>(player_);
  auto* body = world.Get<character::CharacterBody>(player_);
  auto* vm = world.Get<character::CharacterViewMode>(player_);
  auto* inv = world.Get<inventory::Inventory>(player_);
  if (!shape || !move || !state) return;

  ImGui::SetNextWindowSize(ImVec2(340, 820), ImGuiCond_FirstUseEver);
  const f32 panel_x = std::max(20.0f, ImGui::GetIO().DisplaySize.x - 360.0f);
  ImGui::SetNextWindowPos(ImVec2(panel_x, 20), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Gym - character tuning")) {
    const char* stance = state->stance == character::CharacterStance::kCrouching ? "crouch" : "stand";
    const bool tp = vm && vm->kind == character::CharacterViewKind::kThirdPerson;
    ImGui::Text("view: %s   stance: %s   %s", tp ? "third-person" : "first-person", stance,
                state->grounded ? "grounded" : "airborne");
    const f32 speed = std::sqrt(state->velocity.x * state->velocity.x +
                                state->velocity.z * state->velocity.z);
    ImGui::Text("speed: %.2f m/s   eye: %.2f m   crouch: %.0f%%", speed, state->eye_height,
                state->crouch_blend * 100.0f);
    // Feel readout: body facing vs raw look, buffered-jump/coyote windows, dip.
    const f32 look_deg = state->yaw * 180.0f / kPi;
    const f32 face_deg = state->facing_yaw * 180.0f / kPi;
    ImGui::Text("look %+.0f  facing %+.0f%s   gait tgt %.2f m/s", look_deg, face_deg,
                state->pivoting ? " (pivot)" : "", state->gait_speed);
    ImGui::Text("buffer %.02fs  coyote %.02fs  dip %.03f m", state->jump_buffer_timer,
                state->time_since_grounded, state->landing_dip);
    if (body) ImGui::Text("capsule: r %.2f  half %.2f m", body->radius, body->half_height);
    ImGui::Text("cursor: %s (Tab)   inventory crates: %u", mouse_captured_ ? "captured" : "free",
                inv ? inventory::InventoryCount(*inv, crate_def_) : 0);

    // Jetpack: on/off, a fuel bar and the actual (spooled) thrust. No auto-hover —
    // matching thrust to weight to hang still is the player's finesse.
    if (auto* jst = world.Get<character::JetpackState>(player_)) {
      auto* jin = world.Get<character::JetpackInput>(player_);
      const bool on = jin && jin->enabled;
      const char* tag = jst->refueling ? "  refuel" : (jst->burning ? "  BURN" : "");
      ImGui::Text("jetpack: %s%s   (J toggle, hold Space to burn)", on ? "ON" : "off", tag);
      char label[32];
      std::snprintf(label, sizeof(label), "fuel %.0f%%", jst->fuel * 100.0f);
      ImGui::ProgressBar(jst->fuel, ImVec2(-1.0f, 0.0f), label);
      ImGui::Text("thrust %.0f%%", jst->thrust * 100.0f);
    }
    ImGui::Separator();

    if (ImGui::Button("Reset (R)")) ResetPlayer();
    ImGui::SameLine();
    if (ImGui::Button("Toggle view (V)"))
      character::ToggleCharacterViewMode(world, player_, camera_output_, player_, view_settings_,
                                         {.duration = 0.25f});
    ImGui::SameLine();
    if (ImGui::Button("Drop (G)")) DropCrate();

    if (ImGui::CollapsingHeader("Eye height", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::SliderFloat("standing eye", &shape->standing_eye_height, 0.6f, 2.1f, "%.2f m");
      ImGui::SliderFloat("crouched eye", &shape->crouched_eye_height, 0.4f, 1.5f, "%.2f m");
    }
    if (ImGui::CollapsingHeader("Capsule", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::SliderFloat("stand radius", &shape->standing_radius, 0.15f, 0.6f, "%.2f m");
      ImGui::SliderFloat("stand height", &shape->standing_height, 1.0f, 2.4f, "%.2f m");
      ImGui::SliderFloat("crouch radius", &shape->crouched_radius, 0.15f, 0.6f, "%.2f m");
      ImGui::SliderFloat("crouch height", &shape->crouched_height, 0.7f, 1.8f, "%.2f m");
      ImGui::SliderFloat("crouch blend", &shape->crouch_blend_speed, 1.0f, 20.0f, "%.0f /s");
    }
    if (ImGui::CollapsingHeader("Speeds & jump", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::SliderFloat("walk", &move->walk_speed, 0.5f, 6.0f, "%.1f m/s");
      ImGui::SliderFloat("run", &move->run_speed, 1.0f, 9.0f, "%.1f m/s");
      ImGui::SliderFloat("sprint", &move->sprint_speed, 2.0f, 12.0f, "%.1f m/s");
      ImGui::SliderFloat("crouch spd", &move->crouch_speed, 0.5f, 4.0f, "%.1f m/s");
      ImGui::SliderFloat("jump height", &move->jump_height, 0.2f, 3.0f, "%.2f m");
      ImGui::SliderFloat("step height", &move->step_height, 0.0f, 0.8f, "%.2f m");
      f32 slope_deg = move->max_slope_angle * 180.0f / kPi;
      if (ImGui::SliderFloat("max slope", &slope_deg, 20.0f, 80.0f, "%.0f deg"))
        move->max_slope_angle = slope_deg * kPi / 180.0f;
      if (body) body->configured = false;  // re-push slope/step next step
      ImGui::SliderFloat("gravity", &move->gravity, 6.0f, 30.0f, "%.1f m/s2");
    }
    if (ImGui::CollapsingHeader("Feel (never robotic)", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::TextDisabled("turn smoothing (third person)");
      ImGui::SliderFloat("turn half-life", &move->turn_half_life, 0.0f, 0.4f, "%.03f s");
      ImGui::SliderFloat("pivot half-life", &move->pivot_turn_half_life, 0.0f, 0.4f, "%.03f s");
      f32 pivot_deg = move->pivot_angle * 180.0f / kPi;
      if (ImGui::SliderFloat("pivot angle", &pivot_deg, 60.0f, 180.0f, "%.0f deg"))
        move->pivot_angle = pivot_deg * kPi / 180.0f;
      ImGui::TextDisabled("gait + stop");
      ImGui::SliderFloat("speed blend", &move->speed_blend_time, 0.0f, 0.5f, "%.03f s");
      ImGui::SliderFloat("stop epsilon", &move->stop_speed_epsilon, 0.0f, 0.25f, "%.03f m/s");
      ImGui::SliderFloat("air control", &move->air_control, 0.0f, 1.0f, "%.02f");
      ImGui::TextDisabled("jump forgiveness");
      ImGui::SliderFloat("jump buffer", &move->jump_buffer_time, 0.0f, 0.3f, "%.03f s");
      ImGui::SliderFloat("coyote time", &move->coyote_time, 0.0f, 0.3f, "%.03f s");
      ImGui::TextDisabled("eye step smoothing + landing dip");
      ImGui::SliderFloat("eye step half-life", &shape->eye_step_half_life, 0.0f, 0.25f, "%.03f s");
      ImGui::SliderFloat("dip scale", &shape->landing_dip_scale, 0.0f, 0.1f, "%.03f m per m/s");
      ImGui::SliderFloat("dip max", &shape->landing_dip_max, 0.0f, 0.4f, "%.02f m");
      ImGui::SliderFloat("dip recover", &shape->landing_dip_half_life, 0.01f, 0.3f, "%.03f s");
    }
    if (ImGui::CollapsingHeader("Third-person camera", ImGuiTreeNodeFlags_DefaultOpen)) {
      if (auto* boom = world.Get<scene::CameraBoom>(player_))
        ImGui::SliderFloat("boom dist", &boom->distance, view_settings_.tp_min_distance,
                           view_settings_.tp_max_distance, "%.2f m");
      ImGui::SliderFloat("shoulder", &view_settings_.tp_shoulder_offset, -1.0f, 1.0f, "%.2f m");
      ImGui::SliderFloat("height off", &view_settings_.tp_height_offset, -1.0f, 1.5f, "%.2f m");
      ImGui::SliderFloat("min dist", &view_settings_.tp_min_distance, 0.5f, 4.0f, "%.2f m");
      ImGui::SliderFloat("max dist", &view_settings_.tp_max_distance, 3.0f, 10.0f, "%.2f m");
      ImGui::SliderFloat("obstruct r", &view_settings_.tp_obstruction_radius, 0.05f, 0.6f, "%.2f m");
    }
    if (ImGui::CollapsingHeader("Look")) {
      ImGui::SliderFloat("sensitivity", &look_sensitivity_, 0.0005f, 0.006f, "%.4f");
      ImGui::Checkbox("invert pitch", &invert_pitch_);
    }
  }
  ImGui::End();
#endif
}

}  // namespace rx
