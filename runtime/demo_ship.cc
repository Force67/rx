#include "demo_ship.h"

#include <algorithm>
#include <cmath>

#include "asset/asset_id.h"
#include "asset/material.h"
#include "asset/mesh.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "render/geometry/water_field.h"
#include "scene/components.h"

namespace rx {

namespace {

constexpr f32 kPi = 3.14159265358979f;
constexpr f32 kWaterY = 0.0f;

// Ship-local hull envelope (metres). Waterline sits at local y = 0: the keel is
// below, the deck above, so the whole rig is authored around the same origin the
// dynamic buoyancy body floats at.
constexpr f32 kHalfLen = 9.0f;   // stern (-Z) to bow (+Z)
constexpr f32 kBeam = 2.1f;      // half-beam amidships (+X port, -X starboard)
constexpr f32 kDeckY = 1.35f;    // sheer / deck height above the waterline
constexpr f32 kKeelY = -1.7f;    // keel depth below the waterline

// The wind the mesh.vs cloth sway reads (FrameGlobals default direction). The
// ship is oriented and the sails are billowed along it so they fill from astern.
const Vec3 kWindDir = Normalize(Vec3{0.6f, 0.0f, 0.35f});

struct MeshBuild {
  base::Vector<asset::Vertex> v;
  base::Vector<u32> i;

  u32 Add(const Vec3& p, f32 u, f32 tv, u32 color = 0xffffffffu) {
    asset::Vertex vert{};
    vert.position[0] = p.x;
    vert.position[1] = p.y;
    vert.position[2] = p.z;
    vert.tangent[0] = 1;
    vert.tangent[3] = 1;
    vert.uv[0] = u;
    vert.uv[1] = tv;
    vert.color = color;
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

// Area-weighted smooth normals from the triangle list, plus a default tangent.
void ComputeSmoothNormals(MeshBuild& mb) {
  for (asset::Vertex& vert : mb.v) {
    vert.normal[0] = vert.normal[1] = vert.normal[2] = 0;
  }
  for (u32 t = 0; t + 2 < mb.i.size(); t += 3) {
    u32 ia = mb.i[t], ib = mb.i[t + 1], ic = mb.i[t + 2];
    Vec3 pa{mb.v[ia].position[0], mb.v[ia].position[1], mb.v[ia].position[2]};
    Vec3 pb{mb.v[ib].position[0], mb.v[ib].position[1], mb.v[ib].position[2]};
    Vec3 pc{mb.v[ic].position[0], mb.v[ic].position[1], mb.v[ic].position[2]};
    Vec3 n = Cross(pb - pa, pc - pa);  // area-weighted (not normalized)
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

// A capped tube from a to b (tapering r0->r1). Used for masts, yards, bowsprit.
void AddCylinder(MeshBuild& mb, const Vec3& a, const Vec3& b, f32 r0, f32 r1, u32 seg,
                 u32 color = 0xffffffffu) {
  Vec3 axis = b - a;
  f32 len = Length(axis);
  Vec3 dir = len > 1e-5f ? axis * (1.0f / len) : Vec3{0, 1, 0};
  Quat q = QuatBetween({0, 1, 0}, dir);
  u32 base = static_cast<u32>(mb.v.size());
  for (u32 ring = 0; ring < 2; ++ring) {
    Vec3 center = ring == 0 ? a : b;
    f32 r = ring == 0 ? r0 : r1;
    for (u32 s = 0; s < seg; ++s) {
      f32 t = 2.0f * kPi * static_cast<f32>(s) / seg;
      Vec3 local{std::cos(t) * r, 0, std::sin(t) * r};
      mb.Add(center + Rotate(q, local), static_cast<f32>(s) / seg, static_cast<f32>(ring), color);
    }
  }
  for (u32 s = 0; s < seg; ++s) {
    u32 s1 = (s + 1) % seg;
    mb.Quad(base + s, base + s1, base + seg + s1, base + seg + s);
  }
  // End caps (fans to a centre vertex) so the tube reads solid.
  u32 ca = mb.Add(a, 0.5f, 0.5f, color);
  u32 cb = mb.Add(b, 0.5f, 0.5f, color);
  for (u32 s = 0; s < seg; ++s) {
    u32 s1 = (s + 1) % seg;
    mb.Tri(ca, base + s1, base + s);
    mb.Tri(cb, base + seg + s, base + seg + s1);
  }
}

asset::Mesh Finish(MeshBuild& mb, const char* id, const asset::Material& mat) {
  ComputeSmoothNormals(mb);
  asset::Mesh mesh;
  mesh.id = asset::MakeAssetId(id);
  mesh.lods.emplace_back();
  asset::MeshLod& lod = mesh.lods[0];
  lod.vertices = mb.v;
  lod.indices = mb.i;
  asset::Submesh sm;
  sm.index_count = static_cast<u32>(mb.i.size());
  sm.material = mat.id;
  lod.submeshes.push_back(sm);
  // Bounds from the local extent (a loose sphere is enough for cull).
  f32 r = 0;
  for (const asset::Vertex& vert : mb.v) {
    r = std::max(r, std::sqrt(vert.position[0] * vert.position[0] +
                              vert.position[1] * vert.position[1] +
                              vert.position[2] * vert.position[2]));
  }
  mesh.bounds_radius = r;
  return mesh;
}

}  // namespace

ShipDemo::ShipDemo(EngineContext& ctx)
    : ctx_(ctx),
      world_(*ctx.world),
      renderer_(*ctx.renderer),
      camera_(*ctx.camera),
      physics_(*ctx.physics),
      config_(*ctx.config) {}

f32 ShipDemo::Rand() {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return static_cast<f32>(rng_ & 0xffffffu) / 16777216.0f;
}

void ShipDemo::Create() {
  // --- Ocean sheet + deep floor (mirrors the water demo's minimal sea). -------
  asset::Material water_mat;
  water_mat.id = asset::MakeAssetId("ship/water_mat");
  water_mat.base_color_factor[0] = 0.06f;
  water_mat.base_color_factor[1] = 0.11f;
  water_mat.base_color_factor[2] = 0.15f;
  water_mat.base_color_factor[3] = 0.75f;
  water_mat.metallic_factor = 0;
  water_mat.roughness_factor = 0.05f;
  water_mat.alpha_mode = asset::AlphaMode::kBlend;
  water_mat.two_sided = true;
  water_mat.is_water = true;

  asset::Mesh water;
  water.id = asset::MakeAssetId("ship/water");
  water.lods.emplace_back();
  asset::MeshLod& wl = water.lods[0];
  constexpr f32 kHalf = 160.0f;
  constexpr u32 kGrid = 256;
  for (u32 gy = 0; gy <= kGrid; ++gy) {
    for (u32 gx = 0; gx <= kGrid; ++gx) {
      asset::Vertex v{};
      v.position[0] = -kHalf + 2.0f * kHalf * static_cast<f32>(gx) / kGrid;
      v.position[1] = 0;
      v.position[2] = -kHalf + 2.0f * kHalf * static_cast<f32>(gy) / kGrid;
      v.normal[1] = 1;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.uv[0] = v.position[0] / 8.0f;
      v.uv[1] = v.position[2] / 8.0f;
      v.color = 0xffffffff;
      wl.vertices.push_back(v);
    }
  }
  for (u32 gy = 0; gy < kGrid; ++gy) {
    for (u32 gx = 0; gx < kGrid; ++gx) {
      u32 a = gy * (kGrid + 1) + gx, b = a + 1, c = a + (kGrid + 1), d = c + 1;
      for (u32 idx : {a, b, c, b, d, c}) wl.indices.push_back(idx);
    }
  }
  asset::Submesh wsm;
  wsm.index_count = static_cast<u32>(wl.indices.size());
  wsm.material = water_mat.id;
  wl.submeshes.push_back(wsm);
  water.bounds_radius = kHalf * 1.5f;

  asset::Material floor_mat;
  floor_mat.id = asset::MakeAssetId("ship/floor_mat");
  floor_mat.base_color_factor[0] = 0.05f;
  floor_mat.base_color_factor[1] = 0.06f;
  floor_mat.base_color_factor[2] = 0.05f;
  floor_mat.roughness_factor = 0.95f;
  asset::Mesh floor = asset::MakeCube(60.0f, asset::MakeAssetId("ship/floor"));
  for (asset::MeshLod& l : floor.lods) {
    if (l.submeshes.empty()) l.submeshes.push_back({0, static_cast<u32>(l.indices.size()), floor_mat.id});
    else l.submeshes[0].material = floor_mat.id;
  }

  if (!config_.headless) {
    renderer_.UploadMaterial(water_mat);
    renderer_.UploadMaterial(floor_mat);
    renderer_.UploadMesh(water);
    renderer_.UploadMesh(floor);
  }
  ecs::Entity sheet = world_.Create();
  world_.Add(sheet, scene::Transform{});
  world_.Add(sheet, scene::Renderable{water.id});
  ecs::Entity fe = world_.Create();
  world_.Add(fe, scene::Transform{.position = {0, -90.0f, 0}});
  world_.Add(fe, scene::Renderable{floor.id});

  physics_.set_water_height([](const Vec3&, f32* height, Vec3* flow) {
    *height = kWaterY;
    if (flow) *flow = {};
    return true;
  });

  // --- Vessel geometry --------------------------------------------------------
  BuildFlagship();
  BuildAnchoredShip();
  BuildRopes();

  asset::Material iron;
  iron.id = asset::MakeAssetId("ship/iron_mat");
  iron.base_color_factor[0] = 0.05f;
  iron.base_color_factor[1] = 0.05f;
  iron.base_color_factor[2] = 0.06f;
  iron.metallic_factor = 1.0f;
  iron.roughness_factor = 0.4f;
  asset::Mesh ball = asset::MakeSphere(0.2f, 8, 12, asset::MakeAssetId("ship/ball"));
  ball.lods[0].submeshes[0].material = iron.id;
  ball_mesh_ = ball.id.hash;
  if (!config_.headless) {
    renderer_.UploadMaterial(iron);
    renderer_.UploadMesh(ball);
  }

  // --- Flagship dynamic hull body (floats via the buoyancy callback). ---------
  // A box approximating the submerged hull; density < water so it floats. The
  // parallel water-bodies agent's swell-riding buoyancy hooks in right here.
  const Vec3 half{kBeam * 0.95f, 1.2f, kHalfLen * 0.9f};
  hull_pos_ = {0, 0.1f, -12.0f};
  hull_prev_pos_ = hull_pos_;
  hull_body_ = physics_.AddDynamicBox(hull_pos_, half, 460.0f, {0, 0, 2.0f});

  // --- Scene lighting: a fixed afternoon sun so captures are frame-stable. -----
  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = Normalize(Vec3{-0.45f, -0.72f, 0.28f});
  renderer_.settings().sun_intensity = 4.2f;
  renderer_.settings().sun_color = {1.0f, 0.94f, 0.84f};
  renderer_.settings().ambient = 0.10f;
  // DDGI's coarse probe lattice imprints on the big untextured hull; use the
  // stable environment lighting instead (same call the water demo makes).
  renderer_.settings().ddgi = false;

  camera_.set_position({-22.0f, 8.0f, -22.0f});
  camera_.set_yaw_pitch(0.7f, -0.2f);
  camera_.speed = 8.0f;
  ready_ = true;
  RX_INFO("ship demo: procedural brig, wind sails, verlet rigging, cannon broadsides");
}

// The wooden hull: a lofted U-profile swept along the keel, tapering to a stem
// at the bow and a transom at the stern, capped by a flat deck.
void ShipDemo::BuildFlagship() {
  asset::Material wood;
  wood.id = asset::MakeAssetId("ship/wood_mat");
  wood.base_color_factor[0] = 0.20f;
  wood.base_color_factor[1] = 0.12f;
  wood.base_color_factor[2] = 0.06f;
  wood.roughness_factor = 0.8f;
  wood.metallic_factor = 0.0f;

  asset::Material canvas;
  canvas.id = asset::MakeAssetId("ship/canvas_mat");
  canvas.base_color_factor[0] = 0.86f;
  canvas.base_color_factor[1] = 0.82f;
  canvas.base_color_factor[2] = 0.72f;
  canvas.roughness_factor = 0.9f;
  canvas.metallic_factor = 0.0f;
  canvas.two_sided = true;  // sails are seen from both faces
  canvas.wind = true;       // mesh.vs cloth sway, weighted by uv.y (0 = pinned head)

  if (!config_.headless) {
    renderer_.UploadMaterial(wood);
    renderer_.UploadMaterial(canvas);
  }

  // ----- Hull loft -----
  constexpr u32 kStations = 28;  // along Z
  constexpr u32 kProfile = 18;   // around the U cross-section (port -> keel -> stbd)
  MeshBuild hb;
  auto width_scale = [](f32 z) -> f32 {
    // 1 amidships, tapering to a fine stem at the bow (+Z) and a transom aft.
    f32 tn = z / kHalfLen;  // -1..1
    if (tn > 0) return std::max(0.06f, 1.0f - tn * tn * 0.98f);      // pointed bow
    return std::max(0.55f, 1.0f - tn * tn * 0.45f);                  // fuller transom
  };
  auto sheer = [](f32 z) -> f32 {
    f32 tn = z / kHalfLen;
    return kDeckY + 0.35f * tn * tn;  // deck rises toward bow/stern
  };
  u32 grid_base = 0;
  for (u32 si = 0; si <= kStations; ++si) {
    f32 z = -kHalfLen + 2.0f * kHalfLen * static_cast<f32>(si) / kStations;
    f32 ws = width_scale(z);
    f32 deck = sheer(z);
    for (u32 pj = 0; pj <= kProfile; ++pj) {
      f32 th = kPi * static_cast<f32>(pj) / kProfile;  // 0 = starboard rail .. pi = port rail
      f32 x = -kBeam * ws * std::cos(th);
      f32 y = deck - (deck - kKeelY) * std::pow(std::sin(th), 0.62f);
      hb.Add({x, y, z}, static_cast<f32>(pj) / kProfile, z / 8.0f);
    }
  }
  u32 row = kProfile + 1;
  for (u32 si = 0; si < kStations; ++si) {
    for (u32 pj = 0; pj < kProfile; ++pj) {
      u32 a = grid_base + si * row + pj;
      hb.Quad(a, a + 1, a + row + 1, a + row);
    }
  }
  // Flat deck, recessed just below the sheer so a thin bulwark rim shows. Its
  // vertices are DISTINCT from the hull rails: sharing them would let the tall
  // vertical hull-side faces dominate the smooth normal and shade the deck
  // sideways (a dark cavity). Separate verts get pure up-facing normals.
  {
    base::Vector<u32> dp, ds;  // deck port / starboard edge vertices
    for (u32 si = 0; si <= kStations; ++si) {
      f32 z = -kHalfLen + 2.0f * kHalfLen * static_cast<f32>(si) / kStations;
      f32 ws = width_scale(z);
      f32 dy = sheer(z) - 0.18f;  // deck sits below the rail
      ds.push_back(hb.Add({-kBeam * ws * 0.9f, dy, z}, 0.0f, z / 8.0f));
      dp.push_back(hb.Add({kBeam * ws * 0.9f, dy, z}, 1.0f, z / 8.0f));
    }
    for (u32 si = 0; si < kStations; ++si) {
      hb.Quad(ds[si], ds[si + 1], dp[si + 1], dp[si]);
    }
  }
  // Stern transom: fan the aftmost station ring to its centre.
  {
    Vec3 c{0, (sheer(-kHalfLen) + kKeelY) * 0.5f, -kHalfLen};
    u32 cc = hb.Add(c, 0.5f, 0.5f);
    for (u32 pj = 0; pj < kProfile; ++pj) {
      hb.Tri(cc, grid_base + pj, grid_base + pj + 1);
    }
  }
  asset::Mesh hull = Finish(hb, "ship/hull", wood);
  asset::GenerateLods(&hull);  // single-material static -> distance LODs apply
  if (!config_.headless) renderer_.UploadMesh(hull);
  flagship_parts_.push_back({hull.id.hash, Mat4::Identity()});

  // ----- Rig: masts, yards, bowsprit (all wood, one baked mesh) -----
  MeshBuild rb;
  struct MastDef {
    f32 z, height;
  };
  const MastDef masts[2] = {{3.3f, 12.5f}, {-2.2f, 13.5f}};  // fore, main
  for (const MastDef& m : masts) {
    AddCylinder(rb, {0, kDeckY - 0.2f, m.z}, {0, m.height, m.z}, 0.22f, 0.12f, 10);
  }
  // Yards (horizontal spars) at each sail head, spanning the beam.
  struct YardDef {
    f32 z, y, halfspan;
  };
  const YardDef yards[4] = {
      {3.3f, 6.6f, 3.0f}, {3.3f, 10.6f, 2.3f},  // fore: course + topsail
      {-2.2f, 7.0f, 3.3f}, {-2.2f, 11.2f, 2.5f}  // main: course + topsail
  };
  for (const YardDef& y : yards) {
    AddCylinder(rb, {-y.halfspan, y.y, y.z}, {y.halfspan, y.y, y.z}, 0.10f, 0.10f, 8);
  }
  // Bowsprit angled up and forward from the stem.
  AddCylinder(rb, {0, kDeckY, kHalfLen - 0.6f}, {0, 2.8f, kHalfLen + 4.5f}, 0.16f, 0.08f, 8);
  asset::Mesh rig = Finish(rb, "ship/rig", wood);
  if (!config_.headless) renderer_.UploadMesh(rig);
  flagship_parts_.push_back({rig.id.hash, Mat4::Identity()});

  // ----- Sails: billowed square grids hanging below their yards -----
  struct SailDef {
    f32 z, top, bottom, halfw;
    const char* id;
  };
  const SailDef sails[4] = {
      {3.3f, 6.5f, 2.9f, 2.8f, "ship/sail_fc"}, {3.3f, 10.5f, 7.0f, 2.1f, "ship/sail_ft"},
      {-2.2f, 6.9f, 3.0f, 3.1f, "ship/sail_mc"}, {-2.2f, 11.1f, 7.4f, 2.3f, "ship/sail_mt"}};
  constexpr u32 kSw = 12, kSh = 10;
  for (const SailDef& s : sails) {
    MeshBuild sb;
    f32 height = s.top - s.bottom;
    f32 billow = 0.20f * (s.halfw + height);  // bulge scales with the sail size
    for (u32 gy = 0; gy <= kSh; ++gy) {
      f32 b = static_cast<f32>(gy) / kSh;  // 0 head .. 1 foot
      for (u32 gx = 0; gx <= kSw; ++gx) {
        f32 a = static_cast<f32>(gx) / kSw;
        f32 x = (a - 0.5f) * 2.0f * s.halfw;
        f32 y = s.top - b * height;
        // Bulge downwind (+Z), max at centre, fuller toward the foot.
        f32 bulge = std::sin(kPi * a) * (0.25f + 0.75f * b) * billow;
        Vec3 p = Vec3{x, y, s.z} + kWindDir * bulge;
        // uv.y drives the mesh.vs cloth-sway weight (0 = pinned head). Cap the
        // free-edge weight below 1 so the strong default wind flutters the foot
        // without folding the canvas into ragged self-shadowed creases.
        sb.Add(p, a, b * 0.6f);
      }
    }
    u32 sw = kSw + 1;
    for (u32 gy = 0; gy < kSh; ++gy) {
      for (u32 gx = 0; gx < kSw; ++gx) {
        u32 i0 = gy * sw + gx;
        sb.Quad(i0, i0 + 1, i0 + sw + 1, i0 + sw);
      }
    }
    asset::Mesh sail = Finish(sb, s.id, canvas);
    if (!config_.headless) renderer_.UploadMesh(sail);
    flagship_parts_.push_back({sail.id.hash, Mat4::Identity()});
  }

  // Gunports: two rows along the sides at the gun deck, just above the waterline.
  const f32 gz[4] = {-4.0f, -1.3f, 1.4f, 4.1f};
  for (f32 z : gz) {
    ports_.push_back({kBeam * 0.92f, 0.75f, z});   // port rail (+X)
    port_dirs_.push_back({1.0f, 0.06f, 0.0f});
    ports_.push_back({-kBeam * 0.92f, 0.75f, z});  // starboard (-X)
    port_dirs_.push_back({-1.0f, 0.06f, 0.0f});
  }
}

// The distant anchored vessel reuses the flagship meshes at a fixed pose, so it
// exercises the hull's distance LODs and multi-vessel rendering for free.
void ShipDemo::BuildAnchoredShip() {
  Quat yaw = QuatFromAxisAngle({0, 1, 0}, 2.4f);
  anchored_xform_ = MakeTransform(Vec3{52.0f, 0.1f, 20.0f}, yaw, 1.0f);
  for (const Part& p : flagship_parts_) anchored_parts_.push_back(p);
}

// Verlet rigging: stays fore/aft, shrouds to the rails, and sheets from the
// lower yard arms to the deck - a handful of strands, all in ship-local space.
void ShipDemo::BuildRopes() {
  asset::Material tar;
  tar.id = asset::MakeAssetId("ship/rope_mat");
  tar.base_color_factor[0] = 0.04f;
  tar.base_color_factor[1] = 0.03f;
  tar.base_color_factor[2] = 0.02f;
  tar.roughness_factor = 0.85f;
  if (!config_.headless) renderer_.UploadMaterial(tar);
  rope_mesh_ = asset::MakeAssetId("ship/ropes").hash;

  auto add_rope = [&](Vec3 a, Vec3 b, u32 segs, bool pin) {
    Rope r;
    r.pin_last = pin;
    r.seg_len = Length(b - a) / segs;
    for (u32 k = 0; k <= segs; ++k) {
      Vec3 p = Lerp(a, b, static_cast<f32>(k) / segs);
      r.pos.push_back(p);
      r.prev.push_back(p);
    }
    ropes_.push_back(std::move(r));
  };

  // Forestays: masthead down to the bowsprit / bow.
  add_rope({0, 12.2f, 3.3f}, {0, 2.6f, kHalfLen + 4.0f}, 14, true);
  add_rope({0, 13.2f, -2.2f}, {0, 12.2f, 3.3f}, 12, true);  // main -> fore stay
  add_rope({0, 13.2f, -2.2f}, {0, kDeckY, -kHalfLen + 1.0f}, 14, true);  // backstay
  // Shrouds: mastheads out to the rails, port and starboard.
  add_rope({0, 12.2f, 3.3f}, {kBeam, kDeckY, 3.3f}, 12, true);
  add_rope({0, 12.2f, 3.3f}, {-kBeam, kDeckY, 3.3f}, 12, true);
  add_rope({0, 13.2f, -2.2f}, {kBeam, kDeckY, -2.2f}, 12, true);
  add_rope({0, 13.2f, -2.2f}, {-kBeam, kDeckY, -2.2f}, 12, true);
  // Sheets: lower yard arms to the deck (free-hanging tails).
  add_rope({3.0f, 6.6f, 3.3f}, {1.6f, kDeckY, 1.0f}, 10, true);
  add_rope({-3.0f, 6.6f, 3.3f}, {-1.6f, kDeckY, 1.0f}, 10, true);
}

void ShipDemo::Emit(f32 dt, render::FrameView& view) {
  if (!ready_) return;
  if (dt > 0.05f) dt = 0.05f;
  time_ += dt;

  UpdateShipMotion(dt, view);
  UpdateSailsAndRopes(dt, view);
  UpdateCannons(dt, view);
  UpdateParticles(dt, view);
  EmitWake(view);
  FollowCamera();

  // Flagship parts ride the hull pose; the anchored ship sits at its fixed pose.
  for (const Part& p : flagship_parts_) {
    view.draws.push_back({p.mesh, hull_xform_, hull_prev_xform_, -1});
  }
  if (rope_mesh_live_) {
    view.draws.push_back({rope_mesh_, hull_xform_, hull_prev_xform_, -1});
  }
  for (const Part& p : anchored_parts_) {
    view.draws.push_back({p.mesh, anchored_xform_, anchored_xform_, -1});
  }
  for (const Projectile& pr : projectiles_) {
    if (!pr.alive) continue;
    Mat4 m = MakeTranslation(pr.pos);
    view.draws.push_back({ball_mesh_, m, m, -1});
  }
}

void ShipDemo::UpdateShipMotion(f32 dt, render::FrameView& view) {
  (void)view;
  Vec3 pos;
  f32 rot[4];
  if (physics_.GetBodyTransform(hull_body_, &pos, rot)) {
    hull_pos_ = pos;
    hull_rot_ = {rot[0], rot[1], rot[2], rot[3]};
  }
  hull_vel_ = (hull_pos_ - hull_prev_pos_) * (1.0f / std::max(dt, 1e-4f));
  hull_prev_pos_ = hull_pos_;
  hull_prev_xform_ = hull_xform_;
  hull_xform_ = MakeTransform(hull_pos_, hull_rot_, 1.0f);

  // Gentle sail thrust through the centre of mass (no torque -> holds heading),
  // regulated to a slow cruise so the wake trail is steady.
  Vec3 fwd = Rotate(hull_rot_, {0, 0, 1});
  f32 fspeed = Dot(hull_vel_, fwd);
  constexpr f32 kCruise = 2.2f;
  constexpr f32 kMass = 460.0f * (2.0f * kBeam * 0.95f) * (2.4f) * (2.0f * kHalfLen * 0.9f);
  if (fspeed < kCruise) {
    physics_.ApplyImpulse(hull_body_, fwd * (kMass * 2.0f * dt));
  }
  // Loop the flagship back so an unattended capture keeps it in frame.
  if (hull_pos_.z > 40.0f) {
    physics_.SetBodyPosition(hull_body_, {hull_pos_.x, hull_pos_.y, -40.0f}, rot);
    hull_pos_.z = -40.0f;
    hull_prev_pos_ = hull_pos_;
  }
}

void ShipDemo::UpdateSailsAndRopes(f32 dt, render::FrameView& view) {
  (void)view;
  // Sails: the baked billow plus mesh.vs's uv.y-weighted wind sway animate the
  // canvas for free - no per-frame geometry work here.

  // Ropes: verlet in ship-local space (gravity + a swaying local wind), then
  // rebuild the ribbon mesh. Local wind ~ the world wind rotated into the hull
  // frame; the ship barely yaws so the constant approximation is fine.
  const f32 h2 = dt * dt;
  Vec3 gravity{0, -9.8f, 0};
  f32 gust = 0.6f + 0.4f * std::sin(time_ * 2.3f);
  Vec3 wind = Rotate(QuatBetween(Rotate(hull_rot_, {0, 0, 1}), {0, 0, 1}), kWindDir) *
              (4.5f * gust);
  MeshBuild rm;
  for (Rope& r : ropes_) {
    const u32 n = static_cast<u32>(r.pos.size());
    for (u32 k = 1; k < n; ++k) {
      if (r.pin_last && k == n - 1) continue;  // endpoint pinned
      Vec3 cur = r.pos[k];
      Vec3 vel = (cur - r.prev[k]) * 0.98f;  // light damping
      r.prev[k] = cur;
      r.pos[k] = cur + vel + (gravity + wind) * h2;
    }
    // Distance constraints (a few relaxation passes hold the strand taut).
    for (u32 pass = 0; pass < 6; ++pass) {
      for (u32 k = 0; k + 1 < n; ++k) {
        Vec3& p0 = r.pos[k];
        Vec3& p1 = r.pos[k + 1];
        Vec3 d = p1 - p0;
        f32 len = Length(d);
        if (len < 1e-5f) continue;
        f32 diff = (len - r.seg_len) / len;
        bool k0_fixed = (k == 0);
        bool k1_fixed = (r.pin_last && k + 1 == n - 1);
        if (k0_fixed && k1_fixed) continue;
        if (k0_fixed) {
          p1 = p1 - d * diff;
        } else if (k1_fixed) {
          p0 = p0 + d * diff;
        } else {
          p0 = p0 + d * (diff * 0.5f);
          p1 = p1 - d * (diff * 0.5f);
        }
      }
    }
    // Ribbon: quads between nodes, width perpendicular to the segment.
    for (u32 k = 0; k + 1 < n; ++k) {
      Vec3 a = r.pos[k], b = r.pos[k + 1];
      Vec3 seg = Normalize(b - a);
      Vec3 side = Cross(seg, {0, 0, 1});
      if (Length(side) < 1e-3f) side = Cross(seg, {1, 0, 0});
      side = Normalize(side) * r.radius;
      u32 i0 = rm.Add(a - side, 0, 0);
      u32 i1 = rm.Add(a + side, 1, 0);
      u32 i2 = rm.Add(b + side, 1, 1);
      u32 i3 = rm.Add(b - side, 0, 1);
      rm.Quad(i0, i1, i2, i3);
    }
  }
  if (!rm.v.empty() && !config_.headless) {
    asset::Material tar;
    tar.id = asset::MakeAssetId("ship/rope_mat");
    asset::Mesh mesh = Finish(rm, "ship/ropes", tar);
    // PITFALL: UploadMesh WaitIdle-stalls on re-upload ("never per frame"), so
    // this is the one deliberate per-frame stall in the demo. The rope mesh is
    // kept tiny for that reason; a dynamic vertex-buffer path would remove it.
    rope_mesh_live_ = renderer_.UploadMesh(mesh);
  }
}

void ShipDemo::UpdateCannons(f32 dt, render::FrameView& view) {
  (void)view;
  cannon_timer_ -= dt;
  if (cannon_timer_ > 0) return;
  cannon_timer_ = 5.0f;  // next broadside

  // Fire every port on the active rail: muzzle flash + smoke + a ballistic ball.
  Vec3 side_local = broadside_side_ == 0 ? Vec3{1, 0, 0} : Vec3{-1, 0, 0};
  for (u32 p = 0; p < ports_.size(); ++p) {
    if (Dot(port_dirs_[p], side_local) < 0.5f) continue;
    Vec3 muzzle = TransformPoint(hull_xform_, ports_[p]);
    Vec3 dir = Normalize(Rotate(hull_rot_, port_dirs_[p]));
    // Muzzle flash: a bright warm additive burst, brief and compact.
    for (int f = 0; f < 8; ++f) {
      Particle fp;
      fp.pos = muzzle + dir * (0.15f + Rand() * 0.35f);
      fp.vel = dir * (3.5f + Rand() * 4.0f) + Vec3{0, Rand() * 0.6f, 0};
      fp.color = {3.2f, 1.7f, 0.6f};
      fp.max_life = fp.life = 0.12f + Rand() * 0.06f;
      fp.size = 0.13f + Rand() * 0.12f;
      particles_.push_back(fp);
    }
    // Powder smoke: soft grey additive puffs that dissipate (dim so the bloom
    // pass does not blow them into cotton-balls).
    for (int f = 0; f < 9; ++f) {
      Particle sp;
      sp.pos = muzzle + dir * (0.3f + Rand() * 0.9f);
      sp.vel = dir * (0.8f + Rand() * 1.2f) + Vec3{(Rand() - 0.5f) * 0.6f, 0.4f + Rand() * 0.6f,
                                                   (Rand() - 0.5f) * 0.6f};
      sp.color = {0.30f, 0.31f, 0.34f};
      sp.max_life = sp.life = 1.1f + Rand() * 0.9f;
      sp.size = 0.22f + Rand() * 0.28f;
      particles_.push_back(sp);
    }
    muzzle_lights_.push_back({muzzle, 0.16f});
    // Ballistic ball.
    Projectile pr;
    pr.alive = true;
    pr.pos = muzzle + dir * 0.6f;
    pr.vel = dir * 15.0f + Vec3{0, 3.2f, 0};
    projectiles_.push_back(pr);
  }
  broadside_side_ ^= 1;
}

void ShipDemo::UpdateParticles(f32 dt, render::FrameView& view) {
  // Additive convention (see the water demo): the life fade is premultiplied
  // into the HDR radiance and alpha stays 1, so fading never causes a death pop.
  view.particles_emissive = true;

  // Muzzle-flash point lights (brief warm glow like the fire demo).
  for (size_t i = 0; i < muzzle_lights_.size();) {
    muzzle_lights_[i].life -= dt;
    if (muzzle_lights_[i].life <= 0) {
      muzzle_lights_[i] = muzzle_lights_.back();
      muzzle_lights_.pop_back();
      continue;
    }
    render::PointLight l;
    l.pos_radius[0] = muzzle_lights_[i].pos.x;
    l.pos_radius[1] = muzzle_lights_[i].pos.y;
    l.pos_radius[2] = muzzle_lights_[i].pos.z;
    l.pos_radius[3] = 10.0f;
    l.color_intensity[0] = 1.0f;
    l.color_intensity[1] = 0.6f;
    l.color_intensity[2] = 0.25f;
    l.color_intensity[3] = 14.0f;
    view.lights.push_back(l);
    ++i;
  }

  // Ballistic balls: integrate under gravity, splash on the surface.
  for (Projectile& pr : projectiles_) {
    if (!pr.alive) continue;
    pr.vel.y -= 9.8f * dt;
    pr.pos = pr.pos + pr.vel * dt;
    if (pr.pos.y <= kWaterY + 0.05f) {
      pr.alive = false;
      // Impact: a strong ripple + a persistent foam patch through the field.
      render::WaterDisturbance d;
      d.position = {pr.pos.x, kWaterY, pr.pos.z};
      d.radius = 2.4f;
      d.ripple_strength = 2.0f;
      d.foam_amount = 3.0f;  // large one-shot injection -> lingering foam patch
      d.velocity_x = pr.vel.x * 0.2f;
      d.velocity_z = pr.vel.z * 0.2f;
      view.water_disturbances.push_back(d);
      // Spray burst: white additive droplets thrown up from the impact.
      for (int s = 0; s < 24; ++s) {
        Particle sp;
        sp.pos = d.position;
        f32 ang = Rand() * 2.0f * kPi;
        f32 spread = Rand() * 2.2f;
        sp.vel = {std::cos(ang) * spread, 3.5f + Rand() * 3.5f, std::sin(ang) * spread};
        sp.color = {0.9f, 1.0f, 1.1f};
        sp.max_life = sp.life = 0.6f + Rand() * 0.5f;
        sp.size = 0.14f + Rand() * 0.14f;
        particles_.push_back(sp);
      }
    }
  }
  projectiles_.erase(std::remove_if(projectiles_.begin(), projectiles_.end(),
                                    [](const Projectile& p) { return !p.alive; }),
                     projectiles_.end());

  // Integrate + emit the additive billboard pool.
  for (size_t i = 0; i < particles_.size();) {
    Particle& p = particles_[i];
    p.life -= dt;
    if (p.life <= 0) {
      particles_[i] = particles_.back();
      particles_.pop_back();
      continue;
    }
    p.vel.y -= 1.5f * dt;  // light gravity; smoke rises then settles
    p.vel = p.vel * (1.0f - 1.2f * dt);  // air drag
    p.pos = p.pos + p.vel * dt;
    ++i;
  }
  view.particles.reserve(view.particles.size() + particles_.size());
  for (const Particle& p : particles_) {
    f32 t = p.life / p.max_life;
    f32 fade = t * t * 0.9f;  // premultiplied into RGB (alpha stays 1)
    render::ParticleInstance inst;
    inst.pos[0] = p.pos.x;
    inst.pos[1] = p.pos.y;
    inst.pos[2] = p.pos.z;
    inst.size = p.size * (1.4f - 0.4f * t);
    inst.color[0] = p.color.x * fade;
    inst.color[1] = p.color.y * fade;
    inst.color[2] = p.color.z * fade;
    inst.color[3] = 1.0f;
    inst.prev_pos[0] = p.pos.x - p.vel.x * dt;
    inst.prev_pos[1] = p.pos.y - p.vel.y * dt;
    inst.prev_pos[2] = p.pos.z - p.vel.z * dt;
    view.particles.push_back(inst);
  }
}

// Hull-waterline wake: bow/stern/side impulses scaled by cruise speed, the same
// WaterDisturbance path the water-demo cubes use, so the field draws the trail.
void ShipDemo::EmitWake(render::FrameView& view) {
  f32 speed = std::sqrt(hull_vel_.x * hull_vel_.x + hull_vel_.z * hull_vel_.z);
  const Vec3 local_pts[5] = {{0, 0, kHalfLen * 0.9f},   // bow
                             {0, 0, -kHalfLen * 0.9f},  // stern
                             {kBeam, 0, 0},             // port beam
                             {-kBeam, 0, 0},            // starboard beam
                             {0, 0, -kHalfLen * 1.3f}};  // trailing foam wake
  for (u32 k = 0; k < 5; ++k) {
    Vec3 w = TransformPoint(hull_xform_, local_pts[k]);
    render::WaterDisturbance d;
    d.position = {w.x, kWaterY, w.z};
    d.radius = k == 4 ? 3.4f : 2.4f;
    d.ripple_strength = std::min(speed * 0.5f + (k == 0 ? 0.4f : 0.0f), 2.0f);
    d.foam_amount = std::min(speed * (k == 0 ? 0.7f : 0.45f), 1.6f);
    d.velocity_x = hull_vel_.x;
    d.velocity_z = hull_vel_.z;
    view.water_disturbances.push_back(d);
  }
}

void ShipDemo::FollowCamera() {
  // Unattended framing: a 3/4 stern chase unless the player is steering.
  if (ctx_.actions) {
    f32 mag = std::fabs(ctx_.actions->axis(Axis::kLookX)) +
              std::fabs(ctx_.actions->axis(Axis::kLookY)) +
              std::fabs(ctx_.actions->axis(Axis::kMoveX)) +
              std::fabs(ctx_.actions->axis(Axis::kMoveY));
    if (mag > 0.05f) return;  // hand control to the fly camera
  }
  Vec3 offset = Rotate(hull_rot_, Vec3{-13.0f, 8.5f, -14.0f});
  Vec3 cam = hull_pos_ + offset;
  Vec3 target = hull_pos_ + Vec3{0, 4.0f, 2.0f};
  Vec3 dir = Normalize(target - cam);
  f32 yaw = std::atan2(dir.x, -dir.z);
  f32 pitch = std::asin(std::clamp(dir.y, -1.0f, 1.0f));
  camera_.set_position(cam);
  camera_.set_yaw_pitch(yaw, pitch);
}

}  // namespace rx
