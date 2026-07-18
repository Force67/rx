#include "demo_scenes.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include <base/option.h>

#include "asset/asset_database.h"
#include "asset/asset_id.h"
#include "audio/audio_system.h"
#include "audio/thunder_synth.h"
#include "asset/materialx.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "core/math.h"
#include "physics/water_waves.h"
#include "render/atmosphere/lightning.h"
#include "render/geometry/hair_groom.h"
#include "scene/components.h"
#include "viewer_input.h"

namespace rx {

// Config overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup.
static base::Option<const char*> Ply{"ply", nullptr, "RX_PLY"};
static base::Option<bool> OitReverse{"oit.reverse", false, "RX_OIT_REVERSE"};
static base::Option<const char*> Mtlx{"mtlx", nullptr, "RX_MTLX"};
DemoScenes::DemoScenes(EngineContext& ctx)
    : ctx_(ctx),
      world_(*ctx.world),
      scheduler_(*ctx.scheduler),
      renderer_(*ctx.renderer),
      camera_(*ctx.camera),
      physics_(*ctx.physics),
      config_(*ctx.config) {}

void DemoScenes::CreateSceneHookDemoScene() {
  // A normal rx-drawn cube at the origin + a ground plane, so the app's custom
  // boxes (recorded through the scene hook) visibly interleave in depth with rx
  // geometry.
  asset::Mesh cube = asset::MakeCube(0.7f, asset::MakeAssetId("builtin/cube"));
  asset::Mesh ground = asset::MakeCube(3.0f, asset::MakeAssetId("builtin/ground"));
  if (!config_.headless) {
    renderer_.UploadMesh(cube);
    renderer_.UploadMesh(ground);
  }
  ecs::Entity center = world_.Create();
  world_.Add(center, scene::Transform{.position = {0.0f, 0.5f, 0.0f}});
  world_.Add(center, scene::Renderable{cube.id});

  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0.0f, -3.1f, 0.0f}});
  world_.Add(floor, scene::Renderable{ground.id});

  // Oblique view of the box row so the boxes fan across the screen while their
  // depth still straddles the rx cube at the origin.
  camera_.set_position({2.5f, 2.0f, 4.0f});
  camera_.set_yaw_pitch(-0.56f, -0.31f);
  camera_.speed = 4.0f;

  if (!config_.headless) {
    scene_hook_ = std::make_unique<SceneHookDemo>();
    if (!scene_hook_->Init(renderer_)) {
      RX_WARN("scenehook demo unavailable; showing the rx geometry only");
      scene_hook_.reset();
    }
  }
}

void DemoScenes::CreateSceneHookRhiDemoScene() {
  // Reuse the scenehook scene layout (rx cube + ground) so the app's own pure-RHI
  // GPU-driven boxes visibly interleave in depth with rx geometry.
  asset::Mesh cube = asset::MakeCube(0.7f, asset::MakeAssetId("builtin/cube"));
  asset::Mesh ground = asset::MakeCube(3.0f, asset::MakeAssetId("builtin/ground"));
  if (!config_.headless) {
    renderer_.UploadMesh(cube);
    renderer_.UploadMesh(ground);
  }
  ecs::Entity center = world_.Create();
  world_.Add(center, scene::Transform{.position = {0.0f, 0.5f, 0.0f}});
  world_.Add(center, scene::Renderable{cube.id});

  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0.0f, -3.1f, 0.0f}});
  world_.Add(floor, scene::Renderable{ground.id});

  camera_.set_position({2.5f, 2.0f, 4.0f});
  camera_.set_yaw_pitch(-0.56f, -0.31f);
  camera_.speed = 4.0f;

  if (!config_.headless) {
    scene_hook_rhi_ = std::make_unique<SceneHookRhiDemo>();
    if (!scene_hook_rhi_->Init(renderer_)) {
      RX_WARN("scenehook-rhi demo unavailable; showing the rx geometry only");
      scene_hook_rhi_.reset();
    }
  }
}

namespace {

// A fake player wandering a lissajous path, dragging its interest bubble
// across the entity field.
struct BubbleAgent {
  f32 time = 0;
  f32 rate_x = 0.3f;
  f32 rate_z = 0.2f;
  f32 extent = 14.0f;
};

}  // namespace

void DemoScenes::CreateBubbleDemoScene() {
  // The streaming-bubble acceptance scene: a field of replicated entities
  // (NetworkId), three wandering "players" carrying InterestBubbles, the
  // InterestMap driven locally each frame. Entities tint to their owner's
  // color, wire spheres draw through rx::net_viz -- the whole bubble feature
  // without a transport in sight.
  asset::Mesh pawn = asset::MakeCube(0.35f, asset::MakeAssetId("bubbles/pawn"));
  asset::Mesh player = asset::MakeCube(0.7f, asset::MakeAssetId("bubbles/player"));
  asset::Mesh ground = asset::MakeCube(24.0f, asset::MakeAssetId("bubbles/ground"));
  if (!config_.headless) {
    renderer_.UploadMesh(pawn);
    renderer_.UploadMesh(player);
    renderer_.UploadMesh(ground);
  }

  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0.0f, -24.35f, 0.0f}});
  world_.Add(floor, scene::Renderable{ground.id});

  // The replicated field: a 13x13 grid of pawns.
  for (int gx = -6; gx <= 6; ++gx) {
    for (int gz = -6; gz <= 6; ++gz) {
      ecs::Entity e = world_.Create();
      world_.Add(e, scene::Transform{.position = {gx * 3.0f, 0.0f, gz * 3.0f}});
      world_.Add(e, scene::Renderable{pawn.id});
      world_.Add(e, net::AllocateNetworkId());
      world_.Add(e, scene::Tint{0x555555});
    }
  }

  // Three players with 8-unit bubbles on offset wander paths.
  for (u32 peer = 1; peer <= 3; ++peer) {
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {peer * 4.0f, 0.6f, 0.0f}});
    world_.Add(e, scene::Renderable{player.id});
    world_.Add(e, net::AllocateNetworkId());
    world_.Add(e, net::InterestBubble{peer, 8.0f});
    world_.Add(e, scene::Tint{net::PeerColor(peer)});
    BubbleAgent agent;
    agent.time = static_cast<f32>(peer) * 2.4f;
    agent.rate_x = 0.23f + 0.07f * static_cast<f32>(peer);
    agent.rate_z = 0.31f - 0.05f * static_cast<f32>(peer);
    world_.Add(e, agent);
  }

  scheduler_.AddSystem(ecs::Stage::kSim, "bubble_agents", [](ecs::World& world, f32 dt) {
    world.Each<BubbleAgent, scene::Transform>(
        [dt](ecs::Entity, BubbleAgent& agent, scene::Transform& t) {
          agent.time += dt;
          t.position[0] = std::sin(agent.time * agent.rate_x) * agent.extent;
          t.position[2] = std::cos(agent.time * agent.rate_z) * agent.extent;
        });
  });

  bubble_map_.Configure(net::InterestConfig{});
  bubbles_enabled_ = true;

  camera_.set_position({0.0f, 16.0f, 24.0f});
  camera_.set_yaw_pitch(0.0f, -0.55f);
  camera_.speed = 10.0f;

  if (!config_.headless) {
    bubble_viz_ = std::make_unique<net::BubbleVisualizer>();
    if (!bubble_viz_->Init(renderer_)) {
      RX_WARN("bubble visualizer unavailable; tinting only");
      bubble_viz_.reset();
    }
  }
  RX_INFO("bubbles demo: 3 wandering players, 169 replicated pawns, RX_NET_BUBBLES=0 hides the spheres");
}

void DemoScenes::EmitBubbles(render::FrameView& view) {
  bubble_map_.Update(world_, ++bubble_tick_);
  // Paint every replicated entity with its owner's color; unowned = grey.
  world_.Each<net::NetworkId, scene::Tint>(
      [&](ecs::Entity, net::NetworkId& id, scene::Tint& tint) {
        const u32 owner = bubble_map_.OwnerOf(id.value);
        tint.rgb = owner == net::kNoPeer ? 0x555555 : net::PeerColor(owner);
      });
  if (bubble_viz_) bubble_viz_->Emit(view, bubble_map_.bubbles());
}

void DemoScenes::Shutdown() {
  if (placement_) {
    placement_->Shutdown();
    placement_.reset();
  }
  if (cloth_ != 0) {
    physics_.RemoveCloth(cloth_);
    cloth_ = 0;
  }
  // Retire service-owning demos while the host's physics and audio systems are
  // still alive. main.cc destroys Host before Viewer, so leaving these until the
  // DemoScenes destructor would make their teardown call through dead services.
  puppet_.reset();
  drive_.reset();
  gym_.reset();
  scene_hook_.reset();
  scene_hook_rhi_.reset();
  bubble_viz_.reset();
}

void DemoScenes::ApplyRenderPolicy() {
  if (cloth_ == 0) return;
  render::RenderSettings& settings = renderer_.settings();
  settings.aa_mode = render::AntiAliasingMode::kNone;
  settings.upscaler = render::UpscalerKind::kNone;
  settings.dynamic_resolution = false;
  settings.motion_blur = false;
  settings.frame_generation = false;
  settings.path_trace = false;
  settings.path_trace_reference = false;
  settings.path_trace_recon = false;
  settings.rt_shadows = false;
  settings.rtao = false;
  settings.rt_reflections = false;
  settings.ddgi = false;
  settings.rcgi = false;
  settings.ssr = false;
  settings.ssgi = false;
  settings.shadow_maps = true;
}

void DemoScenes::EmitToView(f32 dt, render::FrameView& view) {
  // The viewer's clock rewrites the sun on hour steps and would replace the
  // weather scene's staged overcast gloom with a full noon sun (the clock
  // keeps driving so RX_GAME_HOUR still turns the demo to night). The deck's
  // gloom is a cap on the direct light, not an absolute, so re-clamp per frame.
  if (weather_scene_) {
    renderer_.settings().sun_intensity = std::min(renderer_.settings().sun_intensity, 1.3f);
  }
  if (storm_enabled_) UpdateStorm(dt);
  if (scene_hook_) scene_hook_->Emit(dt, view);
  if (scene_hook_rhi_) scene_hook_rhi_->Emit(dt, view);
  if (ship_) ship_->Emit(dt, view);
  if (nav_) nav_->Emit(dt, view);
  if (placement_) placement_->Emit(dt, view);
  if (gym_) gym_->Emit(dt, view);
  if (puppet_) puppet_->Emit(dt, view);
  if (drive_) drive_->Emit(dt, view);
  if (bubbles_enabled_) EmitBubbles(view);
  if (fluid_scene_) EmitFluid(dt, view);
  if (sky_scene_) EmitSky(dt);
  if (cloth_ != 0) EmitCloth(view);
  UpdateParticles(dt, view);
  if (gpu_particle_count_ > 0) {
    view.gpu_particle_count = gpu_particle_count_;
    view.gpu_particle_emitter = gpu_particle_emitter_;
    view.gpu_particle_mode = gpu_particle_mode_;
    view.gpu_particle_radius = gpu_particle_radius_;
    view.gpu_particle_intensity = gpu_particle_intensity_;
    if (gpu_particle_mode_ == 1) {
      // The fire lights its surroundings: a warm point light hovering in the
      // flame body, intensity flickering with layered sines (the rt lighting
      // path traces real shadows from it).
      fire_time_ += dt;
      f32 t = fire_time_;
      f32 flicker = 0.82f + 0.12f * std::sin(t * 11.7f) + 0.06f * std::sin(t * 23.3f + 1.7f) +
                    0.05f * std::sin(t * 5.1f + 0.6f);
      render::PointLight l;
      l.pos_radius[0] = gpu_particle_emitter_.x;
      l.pos_radius[1] = gpu_particle_emitter_.y + 0.55f;
      l.pos_radius[2] = gpu_particle_emitter_.z;
      l.pos_radius[3] = 14.0f;
      l.color_intensity[0] = 1.0f;
      l.color_intensity[1] = 0.55f;
      l.color_intensity[2] = 0.22f;
      l.color_intensity[3] = 9.0f * flicker;
      view.lights.push_back(l);
    }
  }
  if (fur_ball_) {
    view.fur_ball = true;
    view.fur_position = fur_position_;
  }
  if (!oit_instances_.empty()) view.oit = oit_instances_;
  if (!demo_gaussians_.empty()) view.gaussians = demo_gaussians_;
  if (!demo_lights_.empty()) view.lights = demo_lights_;
  if (!demo_decals_.empty()) view.decals = demo_decals_;

  // Strand-hair demo: gusty wind over every groom, and one head swung on a
  // slow orbit to show the moving attachment (its roots and collision proxy
  // ride the transform; the Jolt strand sim catches up), so the hair sways
  // and lags as the head bobs.
  if (!hair_sims_.empty()) {
    hair_time_ += dt;
    Vec3 wind = Vec3{0.5f, 0.0f, 0.25f} * (0.6f + 0.4f * std::sin(hair_time_ * 2.1f)) +
                Vec3{0.0f, 0.15f * std::sin(hair_time_ * 3.7f), 0.0f};
    for (physics::StrandGroomId sim : hair_sims_) physics_.SetStrandGroomWind(sim, wind);
    if (hair_orbit_strands_ != 0) {
      f32 a = hair_time_ * 0.9f;
      // Mostly a head turn (rotation keeps the head under the hair) plus a
      // small bob, so the hair sways without exposing the scalp.
      Vec3 pos{hair_orbit_center_.x + 0.03f * std::sin(a),
               hair_orbit_center_.y + 0.02f * std::sin(a * 1.7f), hair_orbit_center_.z};
      Mat4 m =
          MakeTranslation(pos) * MakeFromQuat(QuatFromAxisAngle({0, 1, 0}, 0.8f * std::sin(a)));
      physics_.SetStrandGroomTransform(hair_orbit_strands_, m, dt);
      renderer_.SetHairGroomTransform(hair_orbit_groom_, m);
    }
  }

  if (locomotion_enabled_) EmitLocomotion(dt, view);
  // RX_WATER_DISTURB=0 mutes the CPU wake disturbances so a clean A/B can show
  // the depth-driven water interaction (RX_WATER_INTERACTION) ringing the
  // floaters on its own, with no CPU-fed foam confusing the picture.
  if (!water_cubes_.empty()) {
    water_time_ += dt;  // wave clock for the Gerstner buoyancy proxy
    const char* disturb = std::getenv("RX_WATER_DISTURB");
    if (!(disturb && disturb[0] == '0')) EmitWaterDisturbances(dt, view);
  }
}

void DemoScenes::EmitWaterDisturbances(f32 dt, render::FrameView& view) {
  // A bobbing/drifting cube drags a wake ripple and throws foam scaled by how
  // hard it moves; a still cube leaves only a faint standing ripple. A moving
  // cube stretches its splat into a directional wake (elongation), and a cube
  // slamming into the surface throws a one-shot splash (foam pulse + spray).
  const f32 inv_dt = 1.0f / std::max(dt, 1e-4f);
  for (u32 i = 0; i < water_cubes_.size(); ++i) {
    Vec3 pos;
    f32 rot[4];
    if (!physics_.GetBodyTransform(water_cubes_[i], &pos, rot)) continue;
    Vec3 vel = (pos - water_cube_prev_[i]) * inv_dt;
    water_cube_prev_[i] = pos;
    water_cube_slam_cd_[i] = std::max(water_cube_slam_cd_[i] - dt, 0.0f);

    // Local wave surface (height + its vertical velocity) under the cube, from
    // the same Gerstner proxy the buoyancy rides, so slam is measured against
    // the MOVING surface rather than a flat plane.
    f32 surface_vy = 0.0f;
    f32 surface = physics::GerstnerWaveHeight(pos.x, pos.z, water_time_, nullptr, &surface_vy);

    f32 horiz = std::sqrt(vel.x * vel.x + vel.z * vel.z);
    f32 vert = std::fabs(vel.y);
    f32 motion = horiz + vert;
    render::WaterDisturbance d;
    d.position = {pos.x, 0.0f, pos.z};
    d.radius = 2.2f;
    // Rates (per second); the field integrates them over the foam time constant.
    d.ripple_strength = std::min(motion * 0.4f, 2.0f);
    d.foam_amount = std::min(std::max(motion - 0.2f, 0.0f) * 0.7f, 1.8f);
    d.velocity_x = vel.x;
    d.velocity_z = vel.z;
    // Faster horizontal motion stretches the wake down-track; cubes carry no
    // meaningful yaw, so no turn skew (a ship would pass its hull yaw rate).
    d.elongation = std::min(horiz * 0.35f, 2.5f);
    d.angular_velocity = 0.0f;
    view.water_disturbances.push_back(d);

    // Impact splash: vertical velocity relative to the local surface exceeding a
    // threshold while the cube is at the waterline is a slam. Emit a stronger
    // one-shot foam + ripple pulse (plus an additive spray burst) and start a
    // cooldown so a single impact fires once, not every frame it stays wet.
    const f32 rel_vy = vel.y - surface_vy;
    const f32 kSlamSpeed = 2.5f;         // m/s of downward closing speed
    const f32 kWaterlineBand = 1.4f;     // cube half-height + margin
    if (rel_vy < -kSlamSpeed && std::fabs(pos.y - surface) < kWaterlineBand &&
        water_cube_slam_cd_[i] <= 0.0f) {
      water_cube_slam_cd_[i] = 0.6f;
      f32 slam = std::min(-rel_vy, 12.0f);
      render::WaterDisturbance s;
      s.position = {pos.x, 0.0f, pos.z};
      s.radius = 3.4f;                   // splashes ring wider than the wake
      s.ripple_strength = slam * 0.9f;   // sharp pressure pulse
      s.foam_amount = std::min(1.5f + slam * 0.25f, 4.0f);  // big whitewater burst
      s.velocity_x = 0.0f;               // radial: an impact has no heading
      s.velocity_z = 0.0f;
      s.elongation = 0.0f;
      s.angular_velocity = 0.0f;
      view.water_disturbances.push_back(s);
      SpawnSplashSpray(pos, surface, slam);
    }
  }
}

void DemoScenes::SpawnSplashSpray(const Vec3& pos, f32 surface, f32 strength) {
  // A small burst of additive white spray for the slam. Reuses the demo
  // particle pool (integrated + emitted by UpdateParticles next frame); the
  // additive convention there premultiplies the life fade into the HDR radiance
  // with alpha = 1, so the fade never dims the additive path.
  if (!particles_enabled_) return;
  auto rnd = [&]() -> f32 {
    particle_seed_ ^= particle_seed_ << 13;
    particle_seed_ ^= particle_seed_ >> 17;
    particle_seed_ ^= particle_seed_ << 5;
    return static_cast<f32>(particle_seed_ & 0xffffffu) / 16777216.0f;
  };
  u32 count = 10u + static_cast<u32>(strength * 3.0f);
  for (u32 s = 0; s < count && demo_particles_.size() < 20000; ++s) {
    DemoParticle p;
    p.position = {pos.x + (rnd() - 0.5f) * 1.6f, surface + 0.1f, pos.z + (rnd() - 0.5f) * 1.6f};
    f32 ang = rnd() * 6.2831853f;
    f32 spread = 0.6f + rnd() * (0.5f + 0.15f * strength);
    p.velocity = {std::cos(ang) * spread, 2.0f + rnd() * (1.0f + 0.25f * strength),
                  std::sin(ang) * spread};
    p.max_life = 0.5f + rnd() * 0.4f;
    p.life = p.max_life;
    p.size = 0.03f + rnd() * 0.03f;
    p.color = {1.3f, 1.45f, 1.6f};  // cool whitewater spray (HDR, premultiplied on emit)
    demo_particles_.push_back(p);
  }
}

namespace {

// A renderable that the demo scene slowly spins, exercising the ECS update path.
struct Spin {
  f32 angle = 0;
  f32 speed = 0.9f;
};

// Hermite smoothstep. Robust to reversed edges (e0 > e1), so a feature can ramp
// in either direction of z below.
inline f32 SmoothStep(f32 e0, f32 e1, f32 x) {
  f32 t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Analytic terrain bed for --demo fluid, world meters (XZ) -> world-Y height.
// Kept a free function so the sim's CPU bed array and the visual terrain mesh
// evaluate the SAME surface. All features are smoothsteps/gaussians (C1, no
// step steeper than ~45 deg) — heightfield solvers ring on near-vertical beds.
// The dam strip is NOT part of this function (the visible dam is boxes, and the
// sim's dam is stamped in RebuildFluidBed only while dam_up_).
f32 FluidDemoBed(f32 x, f32 z) {
  // Lower basin is the datum (~0). Everything is built up from here.
  // Domain-edge rim: the outer ~12 m rises to ~2.5 m on every side so the flood
  // can't visibly spill out of the 128 m domain.
  f32 edge = std::max(std::fabs(x), std::fabs(z));
  f32 h = 2.5f * SmoothStep(52.0f, 64.0f, edge);

  // Upper reservoir plateau (~6 m), gated to the central x band so it walls the
  // bowl without reaching under the lava hill off to +x. The z ramp reaches
  // full height by z = -19 — UPSTREAM of the dam band — so the rim beside the
  // channel throat stays above the 5.2 m fill level; a longer ramp (the first
  // cut used -12..-24) leaves a ~4 m shoulder the reservoir quietly drains
  // around, dam or no dam.
  f32 plateau_x = 1.0f - SmoothStep(22.0f, 32.0f, std::fabs(x));
  f32 plateau = 6.0f * SmoothStep(-13.0f, -19.0f, z) * plateau_x;

  // Carve the reservoir bowl into the plateau: floor ~3.5 m, centred (0,-40),
  // radius ~18 m (a smooth radial depression).
  f32 rb = std::sqrt(x * x + (z + 40.0f) * (z + 40.0f));
  f32 bowl = 2.5f * (1.0f - SmoothStep(6.0f, 18.0f, rb));
  h = std::max(h, plateau - bowl);

  // Channel notch: the reservoir's only outlet. A 10 m gap at x in [-5,5] whose
  // floor descends 3.5 m -> 0.5 m as z runs -24 -> -12, min-blended through the
  // plateau lip (1 m feather in x, ramps in/out over ~1 m in z).
  f32 channel_floor = 3.5f - 3.0f * SmoothStep(-24.0f, -12.0f, z);
  f32 cmask = (1.0f - SmoothStep(5.0f, 6.0f, std::fabs(x))) *
              SmoothStep(-25.0f, -24.0f, z) * (1.0f - SmoothStep(-13.0f, -12.0f, z));
  h -= std::max(0.0f, h - channel_floor) * cmask;

  // Lava hill on the +x side: a smooth ridge ~8 m peaking near (35,-30). Its +z
  // flank runs ~30+ m downhill into the lower basin (slope ~15 deg), so the vent
  // near its top feeds a flow that runs, slows and solidifies before pooling.
  f32 hx = x - 35.0f, hz = z + 30.0f;
  f32 hill = 8.0f * std::exp(-(hx * hx + hz * hz) / (2.0f * 12.0f * 12.0f));
  h = std::max(h, hill);

  return h;
}

}  // namespace

void DemoScenes::CreateWaterDemoScene() {
  // Empty map with just water and a few reflectors: the fastest loop for
  // iterating water shading without streaming a game world.
  asset::Material water_material;
  water_material.id = asset::MakeAssetId("demo/water_material");
  water_material.base_color_factor[0] = 0.08f;
  water_material.base_color_factor[1] = 0.12f;
  water_material.base_color_factor[2] = 0.16f;
  water_material.base_color_factor[3] = 0.75f;
  water_material.metallic_factor = 0;
  water_material.roughness_factor = 0.05f;
  water_material.alpha_mode = asset::AlphaMode::kBlend;
  water_material.two_sided = true;
  water_material.is_water = true;

  asset::Mesh water;
  water.id = asset::MakeAssetId("demo/water");
  water.lods.emplace_back();
  asset::MeshLod& lod = water.lods[0];
  // Subdivided grid: the gerstner displacement in mesh.vs needs vertices to
  // move. 1 m spacing resolves the shortest authored wavelength (3.7 m).
  constexpr f32 kHalf = 120.0f;
  constexpr u32 kGrid = 240;
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
      lod.vertices.push_back(v);
    }
  }
  for (u32 gy = 0; gy < kGrid; ++gy) {
    for (u32 gx = 0; gx < kGrid; ++gx) {
      u32 a = gy * (kGrid + 1) + gx;
      u32 b = a + 1;
      u32 c = a + (kGrid + 1);
      u32 d = c + 1;
      for (u32 index : {a, b, c, b, d, c}) lod.indices.push_back(index);
    }
  }
  asset::Submesh submesh;
  submesh.index_count = static_cast<u32>(lod.indices.size());
  submesh.material = water_material.id;
  lod.submeshes.push_back(submesh);
  water.bounds_radius = kHalf * 1.5f;

  asset::Mesh cube = asset::MakeCube(1.0f, asset::MakeAssetId("demo/cube"));
  asset::Mesh ground = asset::MakeCube(40.0f, asset::MakeAssetId("demo/ground"));
  if (!config_.headless) {
    renderer_.UploadMaterial(water_material);
    renderer_.UploadMesh(water);
    renderer_.UploadMesh(cube);
    renderer_.UploadMesh(ground);
  }

  // Sea floor far below, water sheet at origin, an island of cubes. The
  // cubes are rigid bodies light enough to float: jolt buoyancy keeps them
  // bobbing on the sheet.
  ecs::Entity floor = world_.Create();
  // Deep enough that Beer-Lambert absorption swallows the box: with its top at
  // -8 m the clear water showed it as a giant dark rectangle mid-ocean.
  world_.Add(floor, scene::Transform{.position = {0, -78.0f, 0}});
  world_.Add(floor, scene::Renderable{ground.id});
  ecs::Entity sheet = world_.Create();
  world_.Add(sheet, scene::Transform{});
  world_.Add(sheet, scene::Renderable{water.id});
  physics_.AddStaticBox({0, -78.0f, 0}, {40.0f, 40.0f, 40.0f});
  // Wave-riding buoyancy: the surface height (and its horizontal orbital flow)
  // comes from the analytic Gerstner proxy, so the cubes ride the swell instead
  // of bobbing on a flat plane and drift along with the passing waves. Entirely
  // CPU/analytic — no GPU readback. Constants live in physics/water_waves.h,
  // kept in sync with the shader Gerstner field.
  physics_.set_water_height([this](const Vec3& p, f32* height, Vec3* flow) {
    Vec3 f{};
    *height = physics::GerstnerWaveHeight(p.x, p.z, water_time_, &f);
    if (flow) *flow = f;  // carries floaters with the swell (rivers would add net drift)
    return true;
  });
  for (int i = 0; i < 6; ++i) {
    ecs::Entity block = world_.Create();
    f32 angle = static_cast<f32>(i) * 1.047f;
    Vec3 position{std::cos(angle) * 6.0f, 2.0f + (i % 3), std::sin(angle) * 6.0f};
    world_.Add(block, scene::Transform{.position = {position.x, position.y, position.z}});
    world_.Add(block, scene::Renderable{cube.id});
    // Density 450 kg/m^3 against the 1.2x buoyancy factor settles the cubes at
    // ~40% submerged (V_sub/V = density / (1.2 * 1000)) with a little freeboard,
    // and keeps the buoyancy spring stiff enough to track the swell rather than
    // resonate with it. One cube gets an initial horizontal shove so it planes
    // across the swell and carves a visible directional wake; the rest ride the
    // waves and drift with the flow.
    Vec3 initial_velocity = (i == 0) ? Vec3{12.0f, 0.0f, 0.0f} : Vec3{};
    physics::BodyId body =
        physics_.AddDynamicBox(position, {1.0f, 1.0f, 1.0f}, 450.0f, initial_velocity);
    if (body) {
      ctx_.physics_entities->push_back({body, block});
      water_cubes_.push_back(body);
      water_cube_prev_.push_back(position);
      water_cube_slam_cd_.push_back(0.0f);
    }
  }

  // A sparse additive ember fountain directly in front of the nearest cube:
  // pixel-sized sprites over a high-contrast silhouette are the acceptance
  // test for jitter-aligned billboards (unjittered ones flicker here).
  // Volumetric sky clouds are intentionally off: this scene isolates water
  // shading.
  particles_enabled_ = true;
  particle_emitter_ = {-7.0f, 0.8f, 0.0f};
  renderer_.settings().clouds = false;
  // DDGI's low-resolution probe volume can imprint its lattice on the large
  // untextured floaters. This scene validates water geometry, so use the
  // stable environment-lighting path here.
  renderer_.settings().ddgi = false;

  // A procedural sandy island so waves visibly wet its beach. A radial gaussian
  // dome that peaks kPeak above the rest water and dips below it further out, so
  // the waterline sits partway up the slope where the swell can lap in and out.
  // The wetting field (renderer) evaluates the same analytic height, keyed off
  // shore_island below; keep the two in sync.
  // Far enough out that deep water stays under the camera and the floaters;
  // the submerged sandy skirt otherwise brightens the whole foreground sea.
  constexpr f32 kIslandCenterX = 30.0f, kIslandCenterZ = -6.0f;
  constexpr f32 kSigma = 10.0f;   // gaussian falloff (m); gentle slope = wide wet band
  constexpr f32 kPeak = 2.4f;     // height above rest water at the centre (m)
  constexpr f32 kRadius = 22.0f;  // mesh half-extent (m)
  constexpr u32 kIslandGrid = 96;
  asset::Material sand_material;
  sand_material.id = asset::MakeAssetId("demo/sand_material");
  sand_material.base_color_factor[0] = 0.40f;
  sand_material.base_color_factor[1] = 0.34f;
  sand_material.base_color_factor[2] = 0.24f;  // a muted tan; bright sand clips to white here
  sand_material.base_color_factor[3] = 1.0f;
  sand_material.metallic_factor = 0.0f;
  sand_material.roughness_factor = 0.9f;

  asset::Mesh island;
  island.id = asset::MakeAssetId("demo/island");
  island.lods.emplace_back();
  asset::MeshLod& island_lod = island.lods[0];
  for (u32 gy = 0; gy <= kIslandGrid; ++gy) {
    for (u32 gx = 0; gx <= kIslandGrid; ++gx) {
      f32 lx = -kRadius + 2.0f * kRadius * static_cast<f32>(gx) / kIslandGrid;
      f32 lz = -kRadius + 2.0f * kRadius * static_cast<f32>(gy) / kIslandGrid;
      f32 g = std::exp(-(lx * lx + lz * lz) / (2.0f * kSigma * kSigma));
      f32 slope = kPeak * 2.0f * g / (kSigma * kSigma);  // -dh/dr factor
      Vec3 n = Normalize(Vec3{slope * lx, 1.0f, slope * lz});
      asset::Vertex v{};
      v.position[0] = lx;
      v.position[1] = kPeak * (2.0f * g - 1.0f);
      v.position[2] = lz;
      v.normal[0] = n.x;
      v.normal[1] = n.y;
      v.normal[2] = n.z;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.uv[0] = lx / 8.0f;
      v.uv[1] = lz / 8.0f;
      v.color = 0xffffffff;
      island_lod.vertices.push_back(v);
    }
  }
  for (u32 gy = 0; gy < kIslandGrid; ++gy) {
    for (u32 gx = 0; gx < kIslandGrid; ++gx) {
      u32 a = gy * (kIslandGrid + 1) + gx;
      u32 b = a + 1;
      u32 c = a + (kIslandGrid + 1);
      u32 d = c + 1;
      for (u32 index : {a, b, c, b, d, c}) island_lod.indices.push_back(index);
    }
  }
  asset::Submesh island_submesh;
  island_submesh.index_count = static_cast<u32>(island_lod.indices.size());
  island_submesh.material = sand_material.id;
  island_lod.submeshes.push_back(island_submesh);
  island.bounds_radius = kRadius * 1.5f;
  if (!config_.headless) {
    renderer_.UploadMaterial(sand_material);
    renderer_.UploadMesh(island);
  }
  ecs::Entity island_entity = world_.Create();
  world_.Add(island_entity, scene::Transform{.position = {kIslandCenterX, 0.0f, kIslandCenterZ}});
  world_.Add(island_entity, scene::Renderable{island.id});

  // Turn the wetting field on and point it at this beach.
  // RX_SHORE_WETTING=0 must survive as a kill switch for A/B captures even
  // though this scene opts in.
  const char* shore_env = std::getenv("RX_SHORE_WETTING");
  renderer_.settings().shore_wetting = !(shore_env && shore_env[0] == '0');
  renderer_.settings().shore_island[0] = kIslandCenterX;
  renderer_.settings().shore_island[1] = kIslandCenterZ;
  renderer_.settings().shore_island[2] = kSigma;
  renderer_.settings().shore_island[3] = kPeak;

  camera_.set_position({-14.0f, 3.0f, 0.0f});
  camera_.set_yaw_pitch(1.5708f, -0.25f);
  RX_INFO("water demo scene");
}

void DemoScenes::RebuildFluidBed() {
  // (Re)fill the CPU-authoritative bed from FluidDemoBed at cell centres. While
  // dam_up_, stamp the channel-plugging wall (raise to 7 m over x in [-5.5,5.5],
  // z in [-17,-15] with a ~1 m feather); on break we omit the strip and the
  // reservoir floods out through the notch. A 512^2 R32F re-upload is trivial.
  const u32 res = fluid_domain_.resolution;
  const f32 l = fluid_domain_.extent / static_cast<f32>(res);
  fluid_bed_.resize(static_cast<size_t>(res) * res);
  for (u32 j = 0; j < res; ++j) {
    f32 z = fluid_domain_.origin[1] + (static_cast<f32>(j) + 0.5f) * l;
    for (u32 i = 0; i < res; ++i) {
      f32 x = fluid_domain_.origin[0] + (static_cast<f32>(i) + 0.5f) * l;
      f32 h = FluidDemoBed(x, z);
      if (dam_up_) {
        // The wall's flat top must run past the notch shoulders (bed ~5.2-5.9 m
        // at |x| 5.5-6.5): the startup slosh piles water ~1 m above the rest
        // level and spills over anything lower than ~6.5 m there.
        f32 dmx = 1.0f - SmoothStep(7.0f, 8.5f, std::fabs(x));
        f32 dmz = SmoothStep(-18.5f, -17.5f, z) * (1.0f - SmoothStep(-14.5f, -13.5f, z));
        h = std::max(h, 8.5f * dmx * dmz);
      }
      fluid_bed_[static_cast<size_t>(j) * res + i] = h;
    }
  }
  fluid_domain_.bed = fluid_bed_.data();  // resize keeps size; re-point defensively
}

void DemoScenes::CreateFluidDemoScene() {
  // Opt into the optional solver (default off): a 128 m, 512^2 world domain
  // centred on the origin (cell l = 0.25 m).
  renderer_.settings().fluid_sim = true;
  fluid_scene_ = true;
  fluid_domain_.origin[0] = -64.0f;
  fluid_domain_.origin[1] = -64.0f;
  fluid_domain_.extent = 128.0f;
  fluid_domain_.resolution = 512;
  fluid_domain_.ambient_temperature = 20.0f;
  fluid_domain_.bed_version = 1;

  // Bed (with the dam in) + a reservoir pre-filled to level 5.2 m behind it. The
  // initial water is computed from the dam-free bed and confined to the bowl and
  // the channel throat upstream of the dam, so the lower basin starts DRY (the
  // payoff is the flood arriving). max(0, 5.2 - bed) leaves the ~1.7 m pool over
  // the 3.5 m bowl floor and zeroes the walls automatically.
  RebuildFluidBed();
  const u32 res = fluid_domain_.resolution;
  const f32 l = fluid_domain_.extent / static_cast<f32>(res);
  fluid_initial_water_.assign(static_cast<size_t>(res) * res, 0.0f);
  for (u32 j = 0; j < res; ++j) {
    f32 z = fluid_domain_.origin[1] + (static_cast<f32>(j) + 0.5f) * l;
    for (u32 i = 0; i < res; ++i) {
      f32 x = fluid_domain_.origin[0] + (static_cast<f32>(i) + 0.5f) * l;
      f32 rb = std::sqrt(x * x + (z + 40.0f) * (z + 40.0f));
      bool in_reservoir = rb < 19.0f || (std::fabs(x) < 6.0f && z < -18.0f && z > -25.0f);
      if (!in_reservoir) continue;
      f32 depth = 5.2f - FluidDemoBed(x, z);
      if (depth > 0.0f) fluid_initial_water_[static_cast<size_t>(j) * res + i] = depth;
    }
  }
  fluid_domain_.initial_water = fluid_initial_water_.data();

  // Visual terrain mesh over the domain plus a skirt out to +/-80 m (positions
  // beyond the domain clamp to the rim, so the world reads as bounded without a
  // hard edge). Same FluidDemoBed as the sim, WITHOUT the dam strip. NOTE: the
  // lava crust the sim grows (solidified flow raising the fluid-visible surface)
  // is NOT reflected in this static mesh — the fluid surface renderer draws the
  // crust; the terrain is just the bed.
  constexpr f32 kSkirt = 80.0f;
  constexpr u32 kGrid = 288;  // ~0.56 m spacing across the 160 m span
  constexpr f32 kEps = 0.5f;  // central-difference step for normals
  asset::Material rock_material;
  rock_material.id = asset::MakeAssetId("demo/fluid_rock");
  rock_material.base_color_factor[0] = 0.30f;  // brownish-gray, matte
  rock_material.base_color_factor[1] = 0.27f;
  rock_material.base_color_factor[2] = 0.23f;
  rock_material.base_color_factor[3] = 1.0f;
  rock_material.metallic_factor = 0.0f;
  rock_material.roughness_factor = 0.95f;

  asset::Mesh terrain;
  terrain.id = asset::MakeAssetId("demo/fluid_terrain");
  terrain.lods.emplace_back();
  asset::MeshLod& tlod = terrain.lods[0];
  for (u32 gy = 0; gy <= kGrid; ++gy) {
    for (u32 gx = 0; gx <= kGrid; ++gx) {
      f32 x = -kSkirt + 2.0f * kSkirt * static_cast<f32>(gx) / kGrid;
      f32 z = -kSkirt + 2.0f * kSkirt * static_cast<f32>(gy) / kGrid;
      f32 sx = std::clamp(x, -64.0f, 64.0f);  // skirt clamps to the domain edge
      f32 sz = std::clamp(z, -64.0f, 64.0f);
      f32 y = FluidDemoBed(sx, sz);
      Vec3 n = Normalize(Vec3{FluidDemoBed(sx - kEps, sz) - FluidDemoBed(sx + kEps, sz),
                              2.0f * kEps,
                              FluidDemoBed(sx, sz - kEps) - FluidDemoBed(sx, sz + kEps)});
      asset::Vertex v{};
      v.position[0] = x;
      v.position[1] = y;
      v.position[2] = z;
      v.normal[0] = n.x;
      v.normal[1] = n.y;
      v.normal[2] = n.z;
      v.tangent[0] = 1;
      v.tangent[3] = 1;
      v.uv[0] = x / 8.0f;
      v.uv[1] = z / 8.0f;
      v.color = 0xffffffff;
      tlod.vertices.push_back(v);
    }
  }
  for (u32 gy = 0; gy < kGrid; ++gy) {
    for (u32 gx = 0; gx < kGrid; ++gx) {
      u32 a = gy * (kGrid + 1) + gx;
      u32 b = a + 1;
      u32 c = a + (kGrid + 1);
      u32 d = c + 1;
      for (u32 index : {a, b, c, b, d, c}) tlod.indices.push_back(index);
    }
  }
  asset::Submesh tsub;
  tsub.index_count = static_cast<u32>(tlod.indices.size());
  tsub.material = rock_material.id;
  tlod.submeshes.push_back(tsub);
  terrain.bounds_radius = kSkirt * 1.6f;

  // The visible dam: gray box slabs spanning the channel throat. MakeBox leaves
  // its submesh list empty (the mayorhem MakeBox-no-submesh pitfall), so append
  // one carrying the gray material explicitly. Static entities (no physics).
  asset::Material dam_material;
  dam_material.id = asset::MakeAssetId("demo/fluid_dam");
  dam_material.base_color_factor[0] = 0.40f;
  dam_material.base_color_factor[1] = 0.40f;
  dam_material.base_color_factor[2] = 0.42f;
  dam_material.base_color_factor[3] = 1.0f;
  dam_material.metallic_factor = 0.0f;
  dam_material.roughness_factor = 0.8f;
  asset::Mesh dam_box = asset::MakeBox(1.45f, 3.5f, 1.2f, asset::MakeAssetId("demo/fluid_dam_box"));
  dam_box.lods[0].submeshes.push_back(
      {0, static_cast<u32>(dam_box.lods[0].indices.size()), dam_material.id});

  if (!config_.headless) {
    renderer_.UploadMaterial(rock_material);
    renderer_.UploadMesh(terrain);
    renderer_.UploadMaterial(dam_material);
    renderer_.UploadMesh(dam_box);
  }

  ecs::Entity terrain_entity = world_.Create();
  world_.Add(terrain_entity, scene::Transform{});
  world_.Add(terrain_entity, scene::Renderable{terrain.id});

  // Four slabs across x in [-5.6, 5.6] at z = -16, sitting on the channel floor
  // and rising to ~7 m (box half-height 3.5, centre y ~3.2). Tracked so they can
  // sink out of sight when the dam breaks.
  fluid_dam_box_y0_ = 3.2f;
  const f32 box_x[4] = {-4.2f, -1.4f, 1.4f, 4.2f};
  for (f32 bx : box_x) {
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {bx, fluid_dam_box_y0_, -16.0f}});
    world_.Add(e, scene::Renderable{dam_box.id});
    fluid_dam_boxes_.push_back(e);
  }

  // Sun/sky matching the other outdoor demos.
  renderer_.settings().sun_direction = Normalize(Vec3{-0.6f, -0.55f, -0.58f});
  renderer_.settings().sun_intensity = 3.2f;
  renderer_.settings().sun_color = {1.0f, 0.95f, 0.88f};

  // Camera at the channel mouth: reservoir + dam + lower basin all in frame,
  // the lava hill visible off to the right. yaw/pitch aim toward (0, ~2, -15).
  // From the basin's west rim: reservoir + dam center-frame, the flood runs
  // toward the camera, the lava hill on the right edge. (forward() is
  // (sin yaw, sin pitch, -cos yaw), so yaw = atan2(dx, -dz).)
  camera_.set_position({-38.0f, 26.0f, 18.0f});
  camera_.set_yaw_pitch(0.90f, -0.36f);
  camera_.speed = 12.0f;
  RX_INFO("fluid demo scene: dam-break reservoir + lava vent");
}

void DemoScenes::EmitFluid(f32 dt, render::FrameView& view) {
  fluid_time_ += dt;

  // Dam-break trigger: frame RX_FLUID_DAM_FRAME if set (deterministic captures),
  // else the scene-local 10 s mark. On break, rebuild the bed without the strip,
  // bump bed_version so the solver re-uploads, and start sinking the boxes.
  if (dam_up_) {
    bool trigger = false;
    const char* frame_env = std::getenv("RX_FLUID_DAM_FRAME");
    if (frame_env && frame_env[0]) {
      trigger = fluid_frame_ >= static_cast<u64>(std::strtoull(frame_env, nullptr, 10));
    } else {
      trigger = fluid_time_ >= 10.0f;
    }
    if (trigger) {
      dam_up_ = false;
      RebuildFluidBed();
      ++fluid_domain_.bed_version;
      fluid_dam_sink_ = 0.0f;
      RX_INFO("fluid demo: dam break (frame {}, t={:.2f}s)", fluid_frame_, fluid_time_);
    }
  }

  // Sink the visible slabs ~10 m below the bed over ~1.5 s (eased) so the wall
  // drops out of the way as the flood front arrives.
  if (fluid_dam_sink_ >= 0.0f) {
    fluid_dam_sink_ += dt;
    f32 drop = 10.0f * SmoothStep(0.0f, 1.5f, fluid_dam_sink_);
    for (ecs::Entity e : fluid_dam_boxes_) {
      if (auto* t = world_.Get<scene::Transform>(e)) t->position[1] = fluid_dam_box_y0_ - drop;
    }
  }

  // Feed the domain + a persistent lava vent near the hilltop every frame. The
  // rate pulses +/-30% on a slow sine for an organic feel. (initial_water only
  // applies on (re)configure, so leaving the pointer set is harmless.)
  view.fluid_domain = &fluid_domain_;
  render::FluidSource lava;
  lava.position = {35.0f, 0.0f, -30.0f};
  lava.radius = 2.5f;
  lava.rate = 0.35f * (1.0f + 0.3f * std::sin(fluid_time_ * 0.7f));
  lava.fluid = 1;
  lava.temperature = 1250.0f;
  view.fluid_sources.push_back(lava);

  ++fluid_frame_;
}

void DemoScenes::CreateMaterialDemoScene() {
  // A grid of spheres sweeping the extended pbr lobes so each reads against the
  // sun and the procedural sky: clearcoat, anisotropy, sheen, and a plain
  // roughness ramp as a control. One material + mesh per sphere.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/matdemo/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -8.5f, 0}});  // top at y = -0.5
  world_.Add(floor, scene::Renderable{ground.id});

  int counter = 0;
  auto spawn = [&](Vec3 pos, asset::Material mat) {
    std::string tag = "builtin/matdemo/" + std::to_string(counter++);
    mat.id = asset::MakeAssetId(tag + "/mat");
    asset::Mesh sphere = asset::MakeSphere(0.5f, 32, 48, asset::MakeAssetId(tag + "/mesh"));
    sphere.lods[0].submeshes[0].material = mat.id;
    if (!config_.headless) {
      renderer_.UploadMaterial(mat);
      renderer_.UploadMesh(sphere);
    }
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{sphere.id});
  };

  const f32 xs[5] = {-2.8f, -1.4f, 0.0f, 1.4f, 2.8f};
  for (int i = 0; i < 5; ++i) {
    f32 t = static_cast<f32>(i) / 4.0f;
    // Row 1: clearcoat 0..1 over a dark red dielectric.
    asset::Material coat;
    coat.base_color_factor[0] = 0.5f; coat.base_color_factor[1] = 0.04f;
    coat.base_color_factor[2] = 0.04f;
    coat.roughness_factor = 0.45f;
    coat.clearcoat = t;
    coat.clearcoat_roughness = 0.05f;
    spawn({xs[i], 0.0f, 1.0f}, coat);

    // Row 2: anisotropy -1..1 over a brushed metal.
    asset::Material metal;
    metal.base_color_factor[0] = 0.95f; metal.base_color_factor[1] = 0.93f;
    metal.base_color_factor[2] = 0.88f;
    metal.metallic_factor = 1.0f;
    metal.roughness_factor = 0.35f;
    metal.anisotropy = t * 2.0f - 1.0f;
    spawn({xs[i], 0.0f, -1.2f}, metal);

    // Row 3: sheen 0..1 over a matte blue cloth.
    asset::Material cloth;
    cloth.base_color_factor[0] = 0.05f; cloth.base_color_factor[1] = 0.07f;
    cloth.base_color_factor[2] = 0.25f;
    cloth.roughness_factor = 0.9f;
    cloth.sheen_color[0] = t; cloth.sheen_color[1] = t; cloth.sheen_color[2] = t;
    cloth.sheen_roughness = 0.3f;
    spawn({xs[i], 0.0f, -3.4f}, cloth);

    // Row 0 (front): transmission 0..1 glass, refracting the rows behind it.
    asset::Material glass;
    glass.base_color_factor[0] = 0.85f; glass.base_color_factor[1] = 0.95f;
    glass.base_color_factor[2] = 0.92f;
    glass.roughness_factor = 0.05f;
    glass.transmission = t;
    glass.ior = 1.5f;
    spawn({xs[i], 0.0f, 2.2f}, glass);

    // Row 4: subsurface scattering 0..1 over pale waxy skin (moved to the back).
    asset::Material skin;
    skin.base_color_factor[0] = 0.85f; skin.base_color_factor[1] = 0.6f;
    skin.base_color_factor[2] = 0.5f;
    skin.roughness_factor = 0.55f;
    skin.subsurface = t;
    skin.subsurface_color[0] = 0.9f; skin.subsurface_color[1] = 0.2f;
    skin.subsurface_color[2] = 0.12f;
    spawn({xs[i], 0.0f, -7.2f}, skin);

    // Row 5: thin-film iridescence, film thickness sweep over a dark dielectric.
    asset::Material irid;
    irid.base_color_factor[0] = 0.04f; irid.base_color_factor[1] = 0.04f;
    irid.base_color_factor[2] = 0.05f;
    irid.roughness_factor = 0.12f;
    irid.iridescence = 1.0f;
    irid.iridescence_thickness = 250.0f + t * 700.0f;  // 250..950 nm
    spawn({xs[i], 0.0f, -5.6f}, irid);
  }

  camera_.set_position({0.0f, 1.8f, 5.4f});
  camera_.set_yaw_pitch(0.0f, -0.16f);
  camera_.speed = 4.0f;
  RX_INFO("material preview: clearcoat, anisotropy, sheen and roughness sweeps");
}

void DemoScenes::UpdateParticles(f32 dt, render::FrameView& view) {
  if (!particles_enabled_) return;
  if (dt > 0.05f) dt = 0.05f;  // clamp hitches so the fountain never explodes
  auto rnd = [&]() -> f32 {
    particle_seed_ ^= particle_seed_ << 13;
    particle_seed_ ^= particle_seed_ >> 17;
    particle_seed_ ^= particle_seed_ << 5;
    return static_cast<f32>(particle_seed_ & 0xffffffu) / 16777216.0f;
  };

  // Integrate and swap-remove the dead.
  for (size_t i = 0; i < demo_particles_.size();) {
    DemoParticle& p = demo_particles_[i];
    p.life -= dt;
    if (p.life <= 0.0f) {
      demo_particles_[i] = demo_particles_.back();
      demo_particles_.pop_back();
      continue;
    }
    p.velocity.y -= 4.0f * dt;  // gravity
    p.position.x += p.velocity.x * dt;
    p.position.y += p.velocity.y * dt;
    p.position.z += p.velocity.z * dt;
    ++i;
  }

  // Spawn an upward cone of embers at a steady rate.
  particle_spawn_accum_ += 45.0f * dt;
  u32 spawn = static_cast<u32>(particle_spawn_accum_);
  particle_spawn_accum_ -= static_cast<f32>(spawn);
  for (u32 s = 0; s < spawn && demo_particles_.size() < 20000; ++s) {
    DemoParticle p;
    p.position = particle_emitter_;
    f32 ang = rnd() * 6.2831853f;
    f32 spread = rnd() * 0.65f;
    p.velocity = {std::cos(ang) * spread, 2.8f + rnd() * 1.4f, std::sin(ang) * spread};
    p.max_life = 0.8f + rnd() * 0.6f;
    p.life = p.max_life;
    p.size = 0.02f + rnd() * 0.02f;
    p.color = {1.6f, 0.35f + rnd() * 0.25f, 0.03f};  // HDR warm embers
    demo_particles_.push_back(p);
  }

  // Emit live billboards into the frame view.
  view.particles.reserve(demo_particles_.size());
  view.particles_emissive = true;
  for (const DemoParticle& p : demo_particles_) {
    f32 t = p.life / p.max_life;  // 1 at birth, 0 at death
    render::ParticleInstance inst;
    inst.pos[0] = p.position.x;
    inst.pos[1] = p.position.y;
    inst.pos[2] = p.position.z;
    inst.size = p.size * (1.3f - 0.3f * t);
    // Additive-path convention (see particle_emitters.cc): the life fade is
    // premultiplied into the HDR radiance and alpha stays 1. Alpha only weights
    // the motion-vector write there, so a fade authored in alpha never dims the
    // ember - it dies at full brightness between two frames (a visible pop) and
    // loses temporal tracking as it ages.
    f32 fade = t * t * 0.8f;  // fade out over life
    inst.color[0] = p.color.x * fade;
    inst.color[1] = p.color.y * fade;
    inst.color[2] = p.color.z * fade;
    inst.color[3] = 1.0f;
    inst.prev_pos[0] = p.position.x - p.velocity.x * dt;  // one frame back, for the motion vector
    inst.prev_pos[1] = p.position.y - p.velocity.y * dt;
    inst.prev_pos[2] = p.position.z - p.velocity.z * dt;
    view.particles.push_back(inst);
  }
}

void DemoScenes::CreateGaussianDemoScene() {
  // A colored sphere reconstructed from 3D gaussian splats: fibonacci-distributed
  // points on the surface, each an isotropic gaussian tinted by its direction.
  // Demonstrates the non-triangle primitive path projecting and blending splats.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/gsplat/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -8.0f, 0}});  // top at y = 0
  world_.Add(floor, scene::Renderable{ground.id});

  // RX_PLY=<path> loads a real captured splat scene instead of the procedural
  // sphere. The renderer sorts and projects them exactly the same way.
  if (const char* ply = Ply.get()) {
    if (render::LoadGaussianPly(ply, &demo_gaussians_)) {
      camera_.set_position({0.0f, 1.0f, 4.0f});
      camera_.set_yaw_pitch(0.0f, 0.0f);
      camera_.speed = 3.0f;
      RX_INFO("gaussian splat demo: {} splats from {}", demo_gaussians_.size(), ply);
      return;
    }
    RX_WARN("gaussian splat demo: ply load failed, using the procedural sphere");
  }

  const u32 kCount = 12000;
  const f32 radius = 1.6f;
  const f32 golden = 2.39996323f;
  demo_gaussians_.reserve(kCount);
  for (u32 i = 0; i < kCount; ++i) {
    f32 t = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kCount);
    f32 y = 1.0f - 2.0f * t;
    f32 r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    f32 phi = static_cast<f32>(i) * golden;
    Vec3 dir{std::cos(phi) * r, y, std::sin(phi) * r};
    render::GaussianInstance g;
    g.position[0] = dir.x * radius;
    g.position[1] = dir.y * radius + 1.8f;
    g.position[2] = dir.z * radius;
    g.scale[0] = g.scale[1] = g.scale[2] = 0.05f;
    g.rotation[3] = 1.0f;  // identity
    g.color[0] = dir.x * 0.5f + 0.5f;
    g.color[1] = dir.y * 0.5f + 0.5f;
    g.color[2] = dir.z * 0.5f + 0.5f;
    g.opacity = 0.9f;
    demo_gaussians_.push_back(g);
  }

  camera_.set_position({0.0f, 1.9f, 5.2f});
  camera_.set_yaw_pitch(0.0f, -0.04f);
  camera_.speed = 3.0f;
  RX_INFO("gaussian splat demo: {} splats", demo_gaussians_.size());
}

void DemoScenes::CreateLodDemoScene() {
  // A row of identical spheres receding from the camera. Each sphere carries
  // three tessellation levels; the gpu cull selects a coarser lod with distance,
  // so the near sphere is smooth and the far ones turn visibly faceted.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/loddemo/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -8.1f, 0}});  // top at y = -0.1
  world_.Add(floor, scene::Renderable{ground.id});

  asset::Material mat;
  mat.id = asset::MakeAssetId("builtin/loddemo/mat");
  mat.base_color_factor[0] = 0.85f;
  mat.base_color_factor[1] = 0.5f;
  mat.base_color_factor[2] = 0.2f;
  mat.roughness_factor = 0.35f;
  mat.metallic_factor = 0.0f;
  if (!config_.headless) renderer_.UploadMaterial(mat);

  // Three spheres at increasing distance, landing on lod 0 / 1 / 2 in turn.
  const Vec3 pos[3] = {{-1.6f, 0.9f, 4.5f}, {1.5f, 0.9f, 2.0f}, {-1.3f, 0.9f, -0.5f}};
  for (int i = 0; i < 3; ++i) {
    std::string tag = "builtin/loddemo/" + std::to_string(i);
    asset::Mesh sphere = asset::MakeLodSphere(1.2f, asset::MakeAssetId(tag + "/mesh"));
    for (asset::MeshLod& lod : sphere.lods) lod.submeshes[0].material = mat.id;
    if (!config_.headless) renderer_.UploadMesh(sphere);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos[i].x, pos[i].y, pos[i].z}});
    world_.Add(e, scene::Renderable{sphere.id});
  }

  camera_.set_position({0.0f, 1.5f, 6.5f});
  camera_.set_yaw_pitch(0.0f, -0.1f);
  camera_.speed = 4.0f;
  RX_INFO("lod demo: distance-based tessellation, near smooth to far faceted");
}

void DemoScenes::CreateCornellDemoScene() {
  // The classic global-illumination test: a white room with a red left wall and
  // a green right wall, open at the top and front so the sun lights the inside.
  // With gi on, the red and green bounce onto the white floor and inner boxes;
  // with gi off the white surfaces stay neutral. Reads best under --preset low
  // (ssgi) but ddgi shows the same bleed under rt.
  auto mat = [&](const char* tag, f32 r, f32 g, f32 b) {
    asset::Material m;
    m.id = asset::MakeAssetId(tag);
    m.base_color_factor[0] = r;
    m.base_color_factor[1] = g;
    m.base_color_factor[2] = b;
    m.roughness_factor = 0.95f;  // matte, so the bounce reads without specular
    m.metallic_factor = 0.0f;
    if (!config_.headless) renderer_.UploadMaterial(m);
    return m.id;
  };
  asset::AssetId white = mat("builtin/cornell/white", 0.8f, 0.8f, 0.8f);
  asset::AssetId red = mat("builtin/cornell/red", 0.8f, 0.05f, 0.05f);
  asset::AssetId green = mat("builtin/cornell/green", 0.05f, 0.8f, 0.05f);

  int counter = 0;
  auto add = [&](asset::Mesh mesh, asset::AssetId material, Vec3 pos) {
    asset::MeshLod& lod = mesh.lods[0];  // MakeBox leaves the submesh list empty
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{mesh.id});
  };
  auto box = [&](f32 hx, f32 hy, f32 hz) {
    return asset::MakeBox(hx, hy, hz, asset::MakeAssetId("builtin/cornell/" + std::to_string(counter++)));
  };

  add(box(2.0f, 0.1f, 2.0f), white, {0, -0.1f, 0});   // floor (top at y = 0)
  add(box(2.0f, 1.6f, 0.1f), white, {0, 1.5f, -2.0f});  // back wall
  add(box(0.1f, 1.6f, 2.0f), red, {-2.0f, 1.5f, 0});    // left wall (red)
  add(box(0.1f, 1.6f, 2.0f), green, {2.0f, 1.5f, 0});   // right wall (green)
  add(box(0.45f, 0.9f, 0.45f), white, {-0.7f, 0.9f, -0.6f});  // tall box
  add(box(0.45f, 0.45f, 0.45f), white, {0.7f, 0.45f, 0.4f});  // short box

  camera_.set_position({0.0f, 1.5f, 4.7f});
  camera_.set_yaw_pitch(0.0f, -0.12f);
  camera_.speed = 3.0f;
  RX_INFO("cornell box: gi color-bleed test (red/green walls)");
}

void DemoScenes::CreateInteriorDemoScene() {
  // A fully enclosed room (floor, ceiling, four walls) with one door-sized gap in
  // the front wall and a single warm lamp inside. The scene is flagged interior
  // (RenderSettings::interior), so the sun/sky are suppressed in raster and the
  // only correct light is the lamp plus a dim authored ambient. It is exactly the
  // configuration that exposes RCGI's skylight leak: with RX_RCGI_INTERIOR=0 the
  // GI probe/gather rays that escape through the doorway sample the sky cubemap
  // and flood the room; with it on (default) they fall back to the interior
  // ambient and the room stays dark and lamp-lit. The room is also forwarded as
  // an interior volume so the classification (item 9b), relocation (item 10) and
  // probe AO (item 11) all exercise here. Run with RX_RCGI=1.
  auto mat = [&](const char* tag, f32 r, f32 g, f32 b, f32 rough) {
    asset::Material m;
    m.id = asset::MakeAssetId(tag);
    m.base_color_factor[0] = r;
    m.base_color_factor[1] = g;
    m.base_color_factor[2] = b;
    m.roughness_factor = rough;
    m.metallic_factor = 0.0f;
    if (!config_.headless) renderer_.UploadMaterial(m);
    return m.id;
  };
  asset::AssetId white = mat("builtin/interior/white", 0.78f, 0.78f, 0.76f, 0.95f);
  asset::AssetId warm = mat("builtin/interior/warm", 0.80f, 0.35f, 0.12f, 0.9f);   // red-ish wall
  asset::AssetId cool = mat("builtin/interior/cool", 0.12f, 0.30f, 0.80f, 0.9f);   // blue-ish wall

  int counter = 0;
  auto add = [&](asset::Mesh mesh, asset::AssetId material, Vec3 pos) {
    asset::MeshLod& lod = mesh.lods[0];  // MakeBox leaves the submesh list empty
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{mesh.id});
  };
  auto box = [&](f32 hx, f32 hy, f32 hz) {
    return asset::MakeBox(hx, hy, hz,
                          asset::MakeAssetId("builtin/interior/" + std::to_string(counter++)));
  };

  // Shell: 4 m x 3 m x 4 m interior (half-extents 2, 1.5, 2).
  add(box(2.0f, 0.1f, 2.0f), white, {0, -0.1f, 0});   // floor (top at y = 0)
  add(box(2.0f, 0.1f, 2.0f), white, {0, 3.1f, 0});    // ceiling (bottom at y = 3)
  add(box(2.0f, 1.6f, 0.1f), white, {0, 1.5f, -2.0f});  // back wall
  add(box(0.1f, 1.6f, 2.0f), warm, {-2.0f, 1.5f, 0});   // left wall (warm)
  add(box(0.1f, 1.6f, 2.0f), cool, {2.0f, 1.5f, 0});    // right wall (cool)
  // Front wall (z = +2) with a ~1.2 m central door gap: two side segments.
  add(box(0.7f, 1.6f, 0.1f), white, {-1.3f, 1.5f, 2.0f});
  add(box(0.7f, 1.6f, 0.1f), white, {1.3f, 1.5f, 2.0f});
  // A short pillar so relocation/probe-AO have contact geometry to occlude.
  add(box(0.35f, 0.9f, 0.35f), white, {-0.6f, 0.9f, -0.5f});

  // The lamp: a warm omni light near the ceiling. This is the only legitimate
  // light indoors (interior mode zeroes the sun), so any extra brightness is a
  // leak. Colored bounce onto the white floor/pillar reads the GI is working.
  render::PointLight lamp;
  lamp.pos_radius[0] = 0.4f;
  lamp.pos_radius[1] = 2.4f;
  lamp.pos_radius[2] = -0.3f;
  lamp.pos_radius[3] = 7.0f;   // influence radius
  lamp.color_intensity[0] = 1.0f;
  lamp.color_intensity[1] = 0.72f;
  lamp.color_intensity[2] = 0.42f;
  lamp.color_intensity[3] = 6.0f;
  demo_lights_.push_back(lamp);

  // Flag interior + author a dim indoor ambient (the RCGI ray-miss fallback), and
  // forward the room as an interior volume for cross-class rejection.
  auto& s = renderer_.settings();
  s.interior = true;
  s.interior_ambient = {0.015f, 0.015f, 0.02f};
  s.interior_directional_intensity = 0.0f;  // no fill: isolate the lamp + GI
  const render::InteriorVolume room{Vec3{-2.0f, 0.0f, -2.0f}, Vec3{2.0f, 3.0f, 2.0f}};
  renderer_.SetInteriorVolumes(std::span<const render::InteriorVolume>(&room, 1));

  camera_.set_position({0.0f, 1.4f, 1.3f});
  camera_.set_yaw_pitch(0.0f, -0.05f);  // face the back wall from just inside the door
  camera_.speed = 2.5f;
  RX_INFO("interior room: RCGI skylight-leak test (RX_RCGI=1; toggle RX_RCGI_INTERIOR)");
}

void DemoScenes::CreateGpuParticleDemoScene() {
  // A dense ember fountain simulated entirely on the gpu: 200k particles, far
  // past the ~20k the cpu fountain caps at, proving the compute sim runs.
  asset::Mesh ground = asset::MakeCube(12.0f, asset::MakeAssetId("builtin/gpufx/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -12.0f, 0}});  // top at y = 0
  world_.Add(floor, scene::Renderable{ground.id});

  gpu_particle_count_ = 200000;
  gpu_particle_emitter_ = {0.0f, 0.1f, 0.0f};

  camera_.set_position({0.0f, 2.6f, 7.5f});
  camera_.set_yaw_pitch(0.0f, -0.18f);
  camera_.speed = 4.0f;
  RX_INFO("gpu particle demo: {} compute-simulated embers", gpu_particle_count_);
}

void DemoScenes::CreateImposterDemoScene() {
  // Octahedral imposters: a procedural conifer baked into a 4x4 view atlas;
  // a few real instances stand close to the camera, and four thousand
  // billboard imposters carry the treeline to the horizon.
  asset::Mesh tree;
  tree.id = asset::MakeAssetId("builtin/imposters/tree");
  tree.lods.resize(1);
  asset::MeshLod& lod = tree.lods[0];
  auto push_vertex = [&](f32 x, f32 y, f32 z, f32 nx, f32 ny, f32 nz, u32 color) {
    asset::Vertex v{};
    v.position[0] = x; v.position[1] = y; v.position[2] = z;
    f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    v.normal[0] = nx / len; v.normal[1] = ny / len; v.normal[2] = nz / len;
    v.tangent[3] = 1.0f;
    v.color = color;
    lod.vertices.push_back(v);
    return static_cast<u32>(lod.vertices.size() - 1);
  };
  // Trunk: a slim box.
  const u32 kBrown = 0xff1e3a58;  // abgr: warm brown
  const u32 kGreen = 0xff2a6a2e;
  const u32 kGreenDark = 0xff1e4a20;
  auto add_quad = [&](u32 a, u32 b, u32 c, u32 d) {
    lod.indices.push_back(a); lod.indices.push_back(b); lod.indices.push_back(c);
    lod.indices.push_back(a); lod.indices.push_back(c); lod.indices.push_back(d);
  };
  const f32 tw = 0.14f, th = 1.1f;
  for (int s = 0; s < 4; ++s) {
    f32 a0 = s * 1.5708f, a1 = a0 + 1.5708f;
    f32 x0 = std::cos(a0) * tw, z0 = std::sin(a0) * tw;
    f32 x1 = std::cos(a1) * tw, z1 = std::sin(a1) * tw;
    f32 nx = std::cos(a0 + 0.7854f), nz = std::sin(a0 + 0.7854f);
    u32 v0 = push_vertex(x0, 0.0f, z0, nx, 0, nz, kBrown);
    u32 v1 = push_vertex(x1, 0.0f, z1, nx, 0, nz, kBrown);
    u32 v2 = push_vertex(x1, th, z1, nx, 0, nz, kBrown);
    u32 v3 = push_vertex(x0, th, z0, nx, 0, nz, kBrown);
    add_quad(v0, v1, v2, v3);
  }
  // Canopy: three stacked cones of triangle fans.
  const f32 tiers[3][3] = {{1.0f, 1.35f, 2.6f}, {0.75f, 2.2f, 3.5f}, {0.5f, 3.0f, 4.2f}};
  for (const auto& tier : tiers) {
    f32 radius = tier[0], base = tier[1], tip = tier[2];
    const int kSegs = 10;
    for (int s = 0; s < kSegs; ++s) {
      f32 a0 = s * 6.2831853f / kSegs, a1 = (s + 1) * 6.2831853f / kSegs;
      f32 x0 = std::cos(a0) * radius, z0 = std::sin(a0) * radius;
      f32 x1 = std::cos(a1) * radius, z1 = std::sin(a1) * radius;
      f32 am = (a0 + a1) * 0.5f;
      u32 color = (s & 1) ? kGreen : kGreenDark;
      u32 v0 = push_vertex(x0, base, z0, x0, radius * 0.6f, z0, color);
      u32 v1 = push_vertex(x1, base, z1, x1, radius * 0.6f, z1, color);
      u32 v2 = push_vertex(0.0f, tip, 0.0f, std::cos(am), 0.8f, std::sin(am), color);
      lod.indices.push_back(v0);
      lod.indices.push_back(v2);
      lod.indices.push_back(v1);
    }
  }
  lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), asset::AssetId{}});

  // Ground plane.
  asset::Material grass;
  grass.id = asset::MakeAssetId("builtin/imposters/grass");
  grass.base_color_factor[0] = 0.22f;
  grass.base_color_factor[1] = 0.32f;
  grass.base_color_factor[2] = 0.16f;
  grass.roughness_factor = 1.0f;
  if (!config_.headless) renderer_.UploadMaterial(grass);
  asset::Mesh ground =
      asset::MakeBox(400.0f, 0.1f, 400.0f, asset::MakeAssetId("builtin/imposters/ground"));
  ground.lods[0].submeshes.push_back(
      {0, static_cast<u32>(ground.lods[0].indices.size()), grass.id});
  if (!config_.headless) {
    renderer_.UploadMesh(ground);
    renderer_.UploadMesh(tree);
  }
  ecs::Entity gnd = world_.Create();
  world_.Add(gnd, scene::Transform{.position = {0.0f, -0.1f, 0.0f}});
  world_.Add(gnd, scene::Renderable{ground.id});

  // Near ring: real meshes. Far field: imposters.
  u32 seed = 12345u;
  auto next_rand = [&seed]() {
    seed = seed * 1664525u + 1013904223u;
    return static_cast<f32>(seed >> 8) / 16777216.0f;
  };
  std::vector<render::ImposterPass::Instance> instances;
  for (int i = 0; i < 4000; ++i) {
    f32 ang = next_rand() * 6.2831853f;
    f32 dist = 22.0f + next_rand() * 170.0f;
    render::ImposterPass::Instance inst;
    inst.position[0] = std::cos(ang) * dist;
    inst.position[1] = 0.0f;
    inst.position[2] = std::sin(ang) * dist;
    inst.scale = 0.8f + next_rand() * 0.7f;
    instances.push_back(inst);
  }
  for (int i = 0; i < 12; ++i) {
    f32 ang = next_rand() * 6.2831853f;
    f32 dist = 6.0f + next_rand() * 12.0f;
    ecs::Entity t = world_.Create();
    world_.Add(t, scene::Transform{.position = {std::cos(ang) * dist, 0.0f,
                                                std::sin(ang) * dist}});
    world_.Add(t, scene::Renderable{tree.id});
  }
  if (!config_.headless) renderer_.BakeImposter(tree, instances);

  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = {-0.6f, -0.5f, -0.62f};
  renderer_.settings().sun_intensity = 3.0f;
  camera_.set_position({0.0f, 2.2f, 16.0f});
  camera_.set_yaw_pitch(0.0f, -0.04f);
  camera_.speed = 15.0f;
}

namespace {

// The physics strand desc is the groom data's constraint/feel model with the
// buffers passed by pointer; collision shapes are added by the caller.
physics::PhysicsWorld::StrandGroomDesc ToStrandGroomDesc(const render::GroomData& data) {
  physics::PhysicsWorld::StrandGroomDesc desc;
  desc.points = data.points.data();
  desc.strand_count = data.guide_count;
  desc.points_per_strand = render::kGroomPointsPerStrand;
  desc.pins = data.pins.data();
  desc.pin_count = static_cast<u32>(data.pins.size() / 2);
  desc.binds = data.binds.data();
  desc.bind_count = static_cast<u32>(data.binds.size() / 4);
  desc.stretch_compliance = data.sim.stretch_compliance;
  desc.bend_compliance = data.sim.bend_compliance;
  desc.bind_compliance = data.sim.bind_compliance;
  desc.damping = data.sim.damping;
  desc.gravity_factor = data.sim.gravity_factor;
  desc.node_mass = data.sim.node_mass;
  desc.node_radius = data.sim.node_radius;
  desc.max_stretch = data.sim.max_stretch;
  desc.iterations = data.sim.iterations;
  return desc;
}

}  // namespace

void DemoScenes::CreateStrandHairDemoScene() {
  // Strand hair simulated by the Jolt soft-body strand sim: three hairstyles
  // built purely as groom data (loose long hair, a woven braid, a pinned
  // ponytail) on head spheres, tinted distinctly. The ponytail head orbits to
  // prove the moving attachment.
  asset::Material skin;
  skin.id = asset::MakeAssetId("builtin/strands/skin");
  skin.base_color_factor[0] = 0.68f;
  skin.base_color_factor[1] = 0.5f;
  skin.base_color_factor[2] = 0.42f;
  skin.roughness_factor = 0.5f;
  asset::Material stone;
  stone.id = asset::MakeAssetId("builtin/strands/stone");
  stone.base_color_factor[0] = 0.32f;
  stone.base_color_factor[1] = 0.32f;
  stone.base_color_factor[2] = 0.34f;
  stone.roughness_factor = 0.9f;
  if (!config_.headless) {
    renderer_.UploadMaterial(skin);
    renderer_.UploadMaterial(stone);
  }

  const f32 head_radius = 0.085f;
  asset::Mesh floor_m =
      asset::MakeBox(8.0f, 0.1f, 8.0f, asset::MakeAssetId("builtin/strands/floor"));
  floor_m.lods[0].submeshes.push_back(
      {0, static_cast<u32>(floor_m.lods[0].indices.size()), stone.id});
  if (!config_.headless) renderer_.UploadMesh(floor_m);
  ecs::Entity f = world_.Create();
  world_.Add(f, scene::Transform{.position = {0.0f, -0.1f, 0.0f}});
  world_.Add(f, scene::Renderable{floor_m.id});

  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = {-0.7f, -0.45f, -0.55f};
  renderer_.settings().sun_intensity = 3.0f;
  renderer_.settings().sun_color = {1.0f, 0.95f, 0.9f};
  renderer_.settings().dof = false;  // macro shot; gameplay dof just blurs it
  camera_.set_position({0.12f, 1.9f, 0.86f});
  camera_.set_yaw_pitch(-0.06f, -0.38f);
  camera_.speed = 2.0f;

  if (config_.headless) return;

  // Three data-defined hairstyles, tinted distinctly; the last one orbits.
  struct HairSpec {
    render::TestGroomStyle style;
    u32 guides;
    u32 children;
    Vec3 tint;
    bool orbit;
  };
  const HairSpec specs[] = {
      {render::TestGroomStyle::kLoose, 700, 20, {1.0f, 0.95f, 0.85f}, false},
      {render::TestGroomStyle::kBraid, 48, 30, {0.9f, 0.85f, 0.8f}, false},
      {render::TestGroomStyle::kPonytail, 500, 20, {1.1f, 0.95f, 0.62f}, true},
  };
  const f32 xs[] = {-0.62f, 0.0f, 0.62f};

  for (u32 i = 0; i < 3; ++i) {
    const HairSpec& spec = specs[i];
    Vec3 head_center{xs[i], 1.62f, 0.0f};
    render::GroomData data;
    if (!render::BuildTestGroom(spec.style, spec.guides, i + 1, &data)) continue;

    render::GroomParams params;
    params.children_per_guide = spec.children;
    params.clump_radius = 0.0035f;
    params.strand_width = 0.0007f;
    params.tint = spec.tint;
    Mat4 transform = MakeTranslation(head_center);
    u32 id = renderer_.CreateHairGroom(data, params, transform);
    if (id == 0) continue;
    hair_grooms_.push_back(id);

    // The matching physics groom: head sphere + a shoulders capsule as the
    // character collision proxy, bound to the renderer groom for readback.
    physics::PhysicsWorld::StrandGroomDesc desc = ToStrandGroomDesc(data);
    physics::PhysicsWorld::StrandGroomDesc::Sphere head_sphere{data.collision_center,
                                                               data.collision_radius};
    physics::PhysicsWorld::StrandGroomDesc::Capsule shoulders{
        {-0.14f, -0.30f, 0.0f}, {0.14f, -0.30f, 0.0f}, 0.07f};
    desc.spheres = &head_sphere;
    desc.sphere_count = 1;
    desc.capsules = &shoulders;
    desc.capsule_count = 1;
    physics::StrandGroomId sim = physics_.CreateStrandGroom(desc, transform);
    if (sim != 0) {
      hair_sims_.push_back(sim);
      ctx_.hair_bindings->push_back({sim, id});
      if (spec.orbit) {
        hair_orbit_groom_ = id;
        hair_orbit_strands_ = sim;
        hair_orbit_center_ = head_center;
      }
    }

    // Head prop sized to sit just inside the groom's collision sphere so the
    // hair envelops it instead of poking through a bare scalp.
    Vec3 hc = head_center;
    f32 hr = head_radius;
    renderer_.HairGroomHead(id, &hc, &hr);
    asset::Mesh head = asset::MakeSphere(hr * 0.92f, 24, 36,
                                         asset::MakeAssetId(std::string("builtin/strands/head") +
                                                            std::to_string(i)));
    head.lods[0].submeshes.push_back({0, static_cast<u32>(head.lods[0].indices.size()), skin.id});
    renderer_.UploadMesh(head);
    ecs::Entity h = world_.Create();
    world_.Add(h, scene::Transform{.position = {hc.x, hc.y, hc.z}});
    world_.Add(h, scene::Renderable{head.id});
  }
}

void DemoScenes::CreateClothDemoScene() {
  constexpr u32 kWidth = 13;
  constexpr u32 kHeight = 15;
  constexpr u32 kVertexCount = kWidth * kHeight;
  constexpr f32 kSpacing = 0.14f;
  static_assert(kVertexCount <= 256, "the demo uses one u8 skin bone per cloth vertex");

  base::Vector<Vec3> rest;
  base::Vector<f32> uvs;
  base::Vector<u32> pins;
  rest.reserve(kVertexCount);
  uvs.reserve(kVertexCount * 2);
  pins.reserve(kWidth);
  cloth_indices_.clear();
  cloth_indices_.reserve((kWidth - 1) * (kHeight - 1) * 6);
  for (u32 y = 0; y < kHeight; ++y) {
    for (u32 x = 0; x < kWidth; ++x) {
      rest.push_back({(static_cast<f32>(x) - static_cast<f32>(kWidth - 1) * 0.5f) * kSpacing,
                      -static_cast<f32>(y) * kSpacing, 0});
      uvs.push_back(static_cast<f32>(x) / static_cast<f32>(kWidth - 1));
      uvs.push_back(static_cast<f32>(y) / static_cast<f32>(kHeight - 1));
    }
  }
  for (u32 x = 0; x < kWidth; ++x) pins.push_back(x);
  for (u32 y = 0; y + 1 < kHeight; ++y) {
    for (u32 x = 0; x + 1 < kWidth; ++x) {
      const u32 a = y * kWidth + x;
      const u32 b = a + 1;
      const u32 c = a + kWidth;
      const u32 d = c + 1;
      for (u32 index : {a, c, b, b, c, d}) cloth_indices_.push_back(index);
    }
  }

  physics::ClothDesc desc;
  desc.positions = rest.data();
  desc.vertex_count = static_cast<u32>(rest.size());
  desc.indices = cloth_indices_.data();
  desc.index_count = static_cast<u32>(cloth_indices_.size());
  desc.uvs = uvs.data();
  desc.pins = pins.data();
  desc.pin_count = static_cast<u32>(pins.size());
  desc.areal_density = 0.24f;
  desc.shear_compliance = 2.0e-6f;
  desc.bend_compliance = 1.5e-4f;
  desc.iterations = 8;
  desc.damping = 0.07f;
  desc.collision_radius = 0.009f;
  desc.self_collision_distance = 0.018f;
  desc.self_collision_iterations = 3;
  desc.aerodynamic_drag = 1.2f;
  cloth_ = physics_.CreateCloth(desc, MakeTranslation({0, 3.0f, 0}));
  if (cloth_ == 0) {
    RX_WARN("cloth demo unavailable: physics.cloth is disabled or Jolt is unavailable");
    return;
  }
  // The render adapter uses a zero-position bind mesh posed entirely by the
  // skin palette. Disable dynamic BLAS and temporal consumers before upload.
  ApplyRenderPolicy();
  physics_.SetClothWind(cloth_, {0.65f, 0.08f, -2.4f});
  cloth_width_ = kWidth;
  cloth_positions_.resize(kVertexCount);
  cloth_normals_.resize(kVertexCount);
  cloth_lines_.reserve(cloth_indices_.size());

  asset::Material fabric;
  fabric.id = asset::MakeAssetId("builtin/cloth/fabric");
  fabric.base_color_factor[0] = 0.018f;
  fabric.base_color_factor[1] = 0.22f;
  fabric.base_color_factor[2] = 0.25f;
  fabric.roughness_factor = 0.62f;
  fabric.sheen_color[0] = 0.12f;
  fabric.sheen_color[1] = 0.34f;
  fabric.sheen_color[2] = 0.32f;
  fabric.sheen_roughness = 0.72f;
  fabric.two_sided = true;

  asset::Mesh cloth_mesh;
  cloth_mesh.id = asset::MakeAssetId("builtin/cloth/simulation_cage");
  cloth_mesh.skinned = true;
  cloth_mesh.exclude_from_rt = true;
  asset::MeshLod& cloth_lod = cloth_mesh.lods.emplace_back();
  cloth_lod.indices = cloth_indices_;
  cloth_lod.vertices.reserve(kVertexCount);
  cloth_lod.skinning.reserve(kVertexCount);
  for (u32 i = 0; i < kVertexCount; ++i) {
    asset::Vertex vertex{};
    // The per-vertex palette matrix supplies both position and shading frame.
    vertex.normal[2] = 1.0f;
    vertex.tangent[0] = 1.0f;
    vertex.tangent[3] = 1.0f;
    vertex.uv[0] = uvs[i * 2 + 0];
    vertex.uv[1] = uvs[i * 2 + 1];
    cloth_lod.vertices.push_back(vertex);
    asset::SkinnedVertexExtra skin;
    skin.bone_indices[0] = static_cast<u8>(i);
    skin.bone_weights[0] = 255;
    cloth_lod.skinning.push_back(skin);
  }
  cloth_lod.submeshes.push_back(
      {0, static_cast<u32>(cloth_lod.indices.size()), fabric.id});
  cloth_mesh_ = cloth_mesh.id.hash;

  asset::Material frame;
  frame.id = asset::MakeAssetId("builtin/cloth/frame_material");
  frame.base_color_factor[0] = 0.52f;
  frame.base_color_factor[1] = 0.16f;
  frame.base_color_factor[2] = 0.055f;
  frame.metallic_factor = 0.65f;
  frame.roughness_factor = 0.3f;
  asset::Material stage;
  stage.id = asset::MakeAssetId("builtin/cloth/stage_material");
  stage.base_color_factor[0] = 0.055f;
  stage.base_color_factor[1] = 0.065f;
  stage.base_color_factor[2] = 0.07f;
  stage.roughness_factor = 0.92f;
  asset::Material collider;
  collider.id = asset::MakeAssetId("builtin/cloth/collider_material");
  collider.base_color_factor[0] = 0.16f;
  collider.base_color_factor[1] = 0.18f;
  collider.base_color_factor[2] = 0.19f;
  collider.metallic_factor = 0.2f;
  collider.roughness_factor = 0.48f;

  auto add_prop = [&](asset::Mesh mesh, const asset::Material& material, const Vec3& position) {
    if (mesh.lods[0].submeshes.empty()) {
      mesh.lods[0].submeshes.push_back(
          {0, static_cast<u32>(mesh.lods[0].indices.size()), material.id});
    } else {
      for (asset::Submesh& submesh : mesh.lods[0].submeshes) submesh.material = material.id;
    }
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity entity = world_.Create();
    world_.Add(entity, scene::Transform{.position = {position.x, position.y, position.z}});
    world_.Add(entity, scene::Renderable{mesh.id});
  };

  if (!config_.headless) {
    renderer_.UploadMaterial(fabric);
    renderer_.UploadMaterial(frame);
    renderer_.UploadMaterial(stage);
    renderer_.UploadMaterial(collider);
    renderer_.UploadMesh(cloth_mesh);
  }
  add_prop(asset::MakeBox(5.0f, 0.1f, 5.0f, asset::MakeAssetId("builtin/cloth/stage")),
           stage, {0, -0.1f, 0});
  add_prop(asset::MakeBox(1.02f, 0.055f, 0.055f, asset::MakeAssetId("builtin/cloth/top_bar")),
           frame, {0, 3.06f, 0.02f});
  add_prop(asset::MakeBox(0.045f, 1.53f, 0.045f,
                          asset::MakeAssetId("builtin/cloth/support_left")),
           frame, {-1.02f, 1.53f, 0.02f});
  add_prop(asset::MakeBox(0.045f, 1.53f, 0.045f,
                          asset::MakeAssetId("builtin/cloth/support_right")),
           frame, {1.02f, 1.53f, 0.02f});
  add_prop(asset::MakeSphere(0.45f, 24, 36,
                             asset::MakeAssetId("builtin/cloth/collision_sphere")),
           collider, {0.52f, 1.68f, -0.28f});

  physics_.AddStaticBox({0, -0.1f, 0}, {5.0f, 0.1f, 5.0f});
  physics_.AddKinematicCapsule({0.52f, 1.68f, -0.28f}, 0.45f, 0.02f);

  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = Normalize(Vec3{-0.65f, -0.52f, -0.55f});
  renderer_.settings().sun_intensity = 4.0f;
  renderer_.settings().sun_color = {1.0f, 0.91f, 0.80f};
  renderer_.settings().ambient = 0.09f;
  renderer_.settings().dof = false;
  renderer_.settings().clouds = false;
  renderer_.settings().aerial_perspective = 0.0f;
  camera_.set_position({3.2f, 2.15f, 2.6f});
  camera_.set_yaw_pitch(-0.89f, -0.08f);
  camera_.speed = 3.0f;
  RX_INFO("cloth demo: {} vertices, {} triangles, wind + rigid contact",
          kVertexCount, cloth_indices_.size() / 3);
  RX_INFO("cloth demo: raster-only AA/RT/upscaler controls are locked");
}

void DemoScenes::EmitCloth(render::FrameView& view) {
  if (cloth_mesh_ == 0 || cloth_width_ == 0 ||
      !physics_.GetClothPositions(cloth_, cloth_positions_.data(),
                                  static_cast<u32>(cloth_positions_.size()))) {
    return;
  }

  std::fill(cloth_normals_.begin(), cloth_normals_.end(), Vec3{});
  cloth_lines_.clear();
  for (size_t i = 0; i < cloth_indices_.size(); i += 3) {
    const u32 a = cloth_indices_[i + 0];
    const u32 b = cloth_indices_[i + 1];
    const u32 c = cloth_indices_[i + 2];
    const Vec3 normal = Cross(cloth_positions_[b] - cloth_positions_[a],
                              cloth_positions_[c] - cloth_positions_[a]);
    cloth_normals_[a] += normal;
    cloth_normals_[b] += normal;
    cloth_normals_[c] += normal;
    cloth_lines_.push_back({cloth_positions_[a], cloth_positions_[b], 0x84d5c5ff});
    cloth_lines_.push_back({cloth_positions_[b], cloth_positions_[c], 0x84d5c5ff});
    cloth_lines_.push_back({cloth_positions_[c], cloth_positions_[a], 0x84d5c5ff});
  }

  const i32 skin_offset = static_cast<i32>(view.bone_matrices.size());
  for (u32 i = 0; i < cloth_positions_.size(); ++i) {
    Vec3 normal = Normalize(cloth_normals_[i]);
    if (Length(normal) < 1.0e-5f) normal = {0, 0, 1};
    if (Dot(normal, view.camera.eye - cloth_positions_[i]) < 0) {
      normal = normal * -1.0f;
    }
    const u32 x = i % cloth_width_;
    const u32 left = x > 0 ? i - 1 : i;
    const u32 right = x + 1 < cloth_width_ ? i + 1 : i;
    Vec3 tangent = cloth_positions_[right] - cloth_positions_[left];
    tangent = Normalize(tangent - normal * Dot(tangent, normal));
    if (Length(tangent) < 1.0e-5f) tangent = Normalize(Cross({0, 1, 0}, normal));
    if (Length(tangent) < 1.0e-5f) tangent = {1, 0, 0};
    const Vec3 bitangent = Normalize(Cross(normal, tangent));

    Mat4 pose = Mat4::Identity();
    pose.m[0] = tangent.x;
    pose.m[1] = tangent.y;
    pose.m[2] = tangent.z;
    pose.m[4] = bitangent.x;
    pose.m[5] = bitangent.y;
    pose.m[6] = bitangent.z;
    pose.m[8] = normal.x;
    pose.m[9] = normal.y;
    pose.m[10] = normal.z;
    pose.m[12] = cloth_positions_[i].x;
    pose.m[13] = cloth_positions_[i].y;
    pose.m[14] = cloth_positions_[i].z;
    view.bone_matrices.push_back(pose);
  }
  view.draws.push_back(
      {cloth_mesh_, Mat4::Identity(), Mat4::Identity(), skin_offset});
  view.debug_lines_overlay =
      std::span<const render::DebugLine>(cloth_lines_.data(), cloth_lines_.size());
}

void DemoScenes::CreateVirtualGeometryDemoScene() {
  // Virtual geometry showcase: a ~800k-triangle displaced terrain tile pushed
  // through the cluster-DAG LOD path. Clusters tint by id (meshlet.ps), so
  // the per-cluster LOD cut is directly visible: cluster density stays
  // roughly constant in screen space as the camera moves.
  //
  // RX_VGEO_HEIGHTMAP=<file.r32> swaps the analytic tile for a real-world DEM:
  // a raw little-endian float32 height grid (meters), RX_VGEO_HM_SIZE samples
  // per side (default 2048), RX_VGEO_HM_STEP meters between samples (default
  // 30). 2048^2 is a ~8.4M-triangle source mesh.
  u32 kGrid = 640;
  f32 kSize = 300.0f;
  std::vector<f32> dem;
  f32 hm_step = 30.0f;
  if (const char* hm = std::getenv("RX_VGEO_HEIGHTMAP")) {
    u32 n = 2048;
    if (const char* e = std::getenv("RX_VGEO_HM_SIZE")) n = std::max(2, std::atoi(e));
    if (const char* e = std::getenv("RX_VGEO_HM_STEP")) hm_step = std::atof(e);
    std::ifstream f(hm, std::ios::binary);
    if (f) {
      dem.resize(static_cast<size_t>(n) * n);
      f.read(reinterpret_cast<char*>(dem.data()), dem.size() * sizeof(f32));
      if (f.gcount() == static_cast<std::streamsize>(dem.size() * sizeof(f32))) {
        kGrid = n - 1;
        kSize = static_cast<f32>(n - 1) * hm_step;
        RX_INFO("vgeo demo: heightmap {} ({}x{}, {:.1f} km)", hm, n, n, kSize / 1000.0f);
      } else {
        RX_ERROR("vgeo demo: heightmap {} short read, using analytic terrain", hm);
        dem.clear();
      }
    } else {
      RX_ERROR("vgeo demo: heightmap {} unreadable, using analytic terrain", hm);
    }
  }
  asset::Mesh terrain;
  terrain.id = asset::MakeAssetId("builtin/vgeo/terrain");
  terrain.lods.resize(1);
  asset::MeshLod& lod = terrain.lods[0];
  lod.vertices.reserve(static_cast<size_t>(kGrid + 1) * (kGrid + 1));
  auto analytic = [](f32 x, f32 z) {
    return 3.0f * std::sin(x * 0.05f) * std::cos(z * 0.045f) +
           0.8f * std::sin(x * 0.31f + 1.7f) * std::sin(z * 0.27f) +
           0.15f * std::sin(x * 1.7f) * std::cos(z * 1.9f);
  };
  const u32 hm_n = kGrid + 1;
  auto height = [&](f32 wx, f32 wz, u32 xi, u32 zi) {
    if (dem.empty()) return analytic(wx, wz);
    return dem[static_cast<size_t>(zi) * hm_n + xi];
  };
  for (u32 z = 0; z <= kGrid; ++z) {
    for (u32 x = 0; x <= kGrid; ++x) {
      f32 wx = (static_cast<f32>(x) / kGrid - 0.5f) * kSize;
      f32 wz = (static_cast<f32>(z) / kGrid - 0.5f) * kSize;
      asset::Vertex v{};
      v.position[0] = wx;
      v.position[1] = height(wx, wz, x, z);
      v.position[2] = wz;
      f32 e = dem.empty() ? 0.25f : hm_step;
      u32 x0 = x > 0 ? x - 1 : x, x1 = x < kGrid ? x + 1 : x;
      u32 z0 = z > 0 ? z - 1 : z, z1 = z < kGrid ? z + 1 : z;
      f32 hx = height(wx + e, wz, x1, z) - height(wx - e, wz, x0, z);
      f32 hz = height(wx, wz + e, x, z1) - height(wx, wz - e, x, z0);
      f32 nx = -hx / (2.0f * e), nz = -hz / (2.0f * e);
      f32 len = std::sqrt(nx * nx + 1.0f + nz * nz);
      v.normal[0] = nx / len;
      v.normal[1] = 1.0f / len;
      v.normal[2] = nz / len;
      lod.vertices.push_back(v);
    }
  }
  lod.indices.reserve(static_cast<size_t>(kGrid) * kGrid * 6);
  for (u32 z = 0; z < kGrid; ++z) {
    for (u32 x = 0; x < kGrid; ++x) {
      u32 i0 = z * (kGrid + 1) + x;
      u32 i1 = i0 + 1;
      u32 i2 = i0 + (kGrid + 1);
      u32 i3 = i2 + 1;
      lod.indices.push_back(i0);
      lod.indices.push_back(i2);
      lod.indices.push_back(i1);
      lod.indices.push_back(i1);
      lod.indices.push_back(i2);
      lod.indices.push_back(i3);
    }
  }
  if (!config_.headless) renderer_.UploadVirtualGeometryMesh(terrain);
  // RX_VGEO_ALBEDO=<file.rgba> drapes an image over the terrain by planar xz
  // projection: raw RGBA8 full mip chain, RX_VGEO_ALBEDO_SIZE px at mip 0
  // (default 4096). Shown by the default-shaded (debug 0) resolve mode.
  if (const char* al = std::getenv("RX_VGEO_ALBEDO")) {
    u32 an = 4096;
    if (const char* e = std::getenv("RX_VGEO_ALBEDO_SIZE")) an = std::max(1, std::atoi(e));
    size_t bytes = 0;
    for (u32 m = an;; m /= 2) {
      bytes += static_cast<size_t>(m) * m * 4;
      if (m == 1) break;
    }
    std::ifstream f(al, std::ios::binary);
    std::vector<rx::u8> mips(bytes);
    if (f && f.read(reinterpret_cast<char*>(mips.data()), bytes) && !config_.headless) {
      renderer_.SetVirtualGeometryAlbedo({mips.data(), mips.size()}, an, 1.0f / kSize);
    } else {
      RX_ERROR("vgeo demo: albedo {} unreadable or short", al);
    }
  }
  // RX_VGEO_INSTANCES=N tiles the terrain N x N: N^2 x 800k source triangles
  // feed the gpu cull while the rastered count stays bounded by the screen.
  int grid = 1;
  if (const char* env = std::getenv("RX_VGEO_INSTANCES")) grid = std::max(1, std::atoi(env));
  if (grid > 1 && !config_.headless) {
    std::vector<rx::Mat4> instances;
    instances.reserve(static_cast<size_t>(grid) * grid);
    for (int z = 0; z < grid; ++z) {
      for (int x = 0; x < grid; ++x) {
        instances.push_back(rx::MakeTranslation(
            {(x - (grid - 1) * 0.5f) * kSize, 0.0f, (z - (grid - 1) * 0.5f) * kSize}));
      }
    }
    renderer_.SetVirtualGeometryInstances(instances);
    RX_INFO("vgeo demo: {}x{} instances", grid, grid);
  }
  RX_INFO("vgeo demo: {} tris in the source terrain", lod.indices.size() / 3);

  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = {-0.55f, -0.55f, -0.63f};
  renderer_.settings().sun_intensity = 3.0f;
  camera_.set_position({0.0f, 6.0f, 40.0f});
  camera_.set_yaw_pitch(0.0f, -0.12f);
  camera_.speed = 20.0f;
  if (!dem.empty()) {
    // Perch over the terrain center; DEM scenes are tens of kilometers.
    u32 ci = (hm_n / 2) * hm_n + hm_n / 2;
    camera_.set_position({0.0f, dem[ci] + 400.0f, 0.0f});
    camera_.speed = 300.0f;
  }
  // RX_VGEO_CAM="x,y,z,yaw,pitch" pins the fly camera for repeatable captures.
  if (const char* cam = std::getenv("RX_VGEO_CAM")) {
    f32 v[5] = {0, 0, 0, 0, 0};
    if (std::sscanf(cam, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]) == 5) {
      camera_.set_position({v[0], v[1], v[2]});
      camera_.set_yaw_pitch(v[3], v[4]);
    }
  }
}

void DemoScenes::CreateVirtualTextureDemoScene() {
  // Virtual texturing showcase: one huge ground plane whose albedo streams
  // from the feedback-driven page atlas (the megatexture is procedural - a
  // survey grid with mip tinting, so residency and LOD read directly).
  asset::Material vt_mat;
  vt_mat.id = asset::MakeAssetId("builtin/vt/mat");
  vt_mat.virtual_albedo = true;
  vt_mat.roughness_factor = 0.85f;
  if (!config_.headless) renderer_.UploadMaterial(vt_mat);

  asset::Mesh ground =
      asset::MakeBox(240.0f, 0.2f, 240.0f, asset::MakeAssetId("builtin/vt/ground"));
  ground.lods[0].submeshes.push_back(
      {0, static_cast<u32>(ground.lods[0].indices.size()), vt_mat.id});
  // A few reference blocks so scale and shadows read.
  asset::Mesh block = asset::MakeBox(2.0f, 2.0f, 2.0f, asset::MakeAssetId("builtin/vt/block"));
  block.lods[0].submeshes.push_back(
      {0, static_cast<u32>(block.lods[0].indices.size()), vt_mat.id});
  if (!config_.headless) {
    renderer_.UploadMesh(ground);
    renderer_.UploadMesh(block);
  }
  ecs::Entity g = world_.Create();
  world_.Add(g, scene::Transform{.position = {0.0f, -0.2f, 0.0f}});
  world_.Add(g, scene::Renderable{ground.id});
  for (int i = 0; i < 4; ++i) {
    ecs::Entity b = world_.Create();
    world_.Add(b, scene::Transform{.position = {-12.0f + 8.0f * i, 1.0f, -14.0f - 6.0f * i}});
    world_.Add(b, scene::Renderable{block.id});
  }

  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = {-0.5f, -0.6f, -0.62f};
  renderer_.settings().sun_intensity = 3.2f;
  renderer_.settings().sun_color = {1.0f, 0.95f, 0.88f};
  camera_.set_position({0.0f, 1.8f, 8.0f});
  camera_.set_yaw_pitch(0.0f, -0.12f);
  camera_.speed = 12.0f;
}

void DemoScenes::CreateBrickDemoScene() {
  // Parallax-occlusion showcase: a procedurally textured brick wall + floor
  // under a grazing sun, so the mortar recesses parallax and self-shadow.
  constexpr u32 kTex = 256;
  auto brick_height = [](f32 u, f32 v) -> f32 {
    // Two-course running bond with rounded mortar channels.
    f32 row = v * 8.0f;
    f32 col = u * 4.0f + (static_cast<int>(row) % 2 ? 0.5f : 0.0f);
    f32 fy = row - std::floor(row);
    f32 fx = col - std::floor(col);
    auto channel = [](f32 t, f32 w) {
      f32 d = std::min(t, 1.0f - t) / w;  // distance to the mortar line
      return std::min(d, 1.0f);
    };
    f32 h = std::min(channel(fx, 0.06f), channel(fy, 0.10f));
    h = h * h * (3.0f - 2.0f * h);  // rounded shoulder
    // Slight per-brick height variation + surface grain.
    u32 bx = static_cast<u32>(col), by = static_cast<u32>(row);
    u32 seed = bx * 374761393u + by * 668265263u;
    seed = (seed ^ (seed >> 13)) * 1274126177u;
    f32 vary = 0.85f + 0.15f * static_cast<f32>(seed & 0xffu) / 255.0f;
    return h * vary;
  };
  asset::Texture height;
  height.id = asset::MakeAssetId("builtin/bricks/height");
  height.format = asset::TextureFormat::kRgba8;
  height.width = height.height = kTex;
  height.data.resize(static_cast<size_t>(kTex) * kTex * 4);
  asset::Texture albedo = height;
  albedo.id = asset::MakeAssetId("builtin/bricks/albedo");
  albedo.is_srgb = true;
  asset::Texture normal = height;
  normal.id = asset::MakeAssetId("builtin/bricks/normal");
  for (u32 y = 0; y < kTex; ++y) {
    for (u32 x = 0; x < kTex; ++x) {
      f32 u = (x + 0.5f) / kTex, v = (y + 0.5f) / kTex;
      f32 h = brick_height(u, v);
      size_t o = (static_cast<size_t>(y) * kTex + x) * 4;
      height.data[o] = height.data[o + 1] = height.data[o + 2] =
          static_cast<u8>(h * 255.0f);
      height.data[o + 3] = 255;
      // Normal from height finite differences (tangent space, +z out).
      f32 e = 1.0f / kTex;
      f32 hx = brick_height(u + e, v) - brick_height(u - e, v);
      f32 hy = brick_height(u, v + e) - brick_height(u, v - e);
      f32 nx = -hx * 6.0f, ny = -hy * 6.0f, nz = 1.0f;
      f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
      normal.data[o] = static_cast<u8>((nx / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 1] = static_cast<u8>((ny / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 2] = static_cast<u8>((nz / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 3] = 255;
      // Brick red where raised, grey mortar in the channels.
      f32 m = h < 0.55f ? 0.0f : 1.0f;
      u32 bx = static_cast<u32>(u * 4.0f * 7919u), by = static_cast<u32>(v * 8.0f);
      u32 seed = bx * 2654435761u + by * 40503u;
      f32 tint = 0.85f + 0.15f * static_cast<f32>((seed >> 8) & 0xffu) / 255.0f;
      f32 r = m * 0.58f * tint + (1.0f - m) * 0.42f;
      f32 g = m * 0.25f * tint + (1.0f - m) * 0.40f;
      f32 b = m * 0.20f * tint + (1.0f - m) * 0.38f;
      albedo.data[o] = static_cast<u8>(r * 255.0f);
      albedo.data[o + 1] = static_cast<u8>(g * 255.0f);
      albedo.data[o + 2] = static_cast<u8>(b * 255.0f);
      albedo.data[o + 3] = 255;
    }
  }
  if (!config_.headless) {
    renderer_.UploadTexture(height);
    renderer_.UploadTexture(albedo);
    renderer_.UploadTexture(normal);
  }

  asset::Material brick;
  brick.id = asset::MakeAssetId("builtin/bricks/mat");
  brick.base_color = albedo.id;
  brick.normal = normal.id;
  brick.height = height.id;
  brick.height_scale = 0.06f;
  brick.roughness_factor = 0.9f;
  if (!config_.headless) renderer_.UploadMaterial(brick);
  asset::Material brick_flat = brick;  // a/b: same look minus the pom march
  brick_flat.id = asset::MakeAssetId("builtin/bricks/mat_flat");
  brick_flat.height = {};
  if (!config_.headless) renderer_.UploadMaterial(brick_flat);

  // Two walls side by side (pom | flat) + a floor, sun grazing along them.
  asset::Mesh wall = asset::MakeBox(3.0f, 2.0f, 0.15f, asset::MakeAssetId("builtin/bricks/wall"));
  wall.lods[0].submeshes.push_back({0, static_cast<u32>(wall.lods[0].indices.size()), brick.id});
  asset::Mesh wall_flat =
      asset::MakeBox(3.0f, 2.0f, 0.15f, asset::MakeAssetId("builtin/bricks/wall_flat"));
  wall_flat.lods[0].submeshes.push_back(
      {0, static_cast<u32>(wall_flat.lods[0].indices.size()), brick_flat.id});
  asset::Mesh floor_mesh =
      asset::MakeBox(14.0f, 0.15f, 8.0f, asset::MakeAssetId("builtin/bricks/floor"));
  floor_mesh.lods[0].submeshes.push_back(
      {0, static_cast<u32>(floor_mesh.lods[0].indices.size()), brick.id});
  if (!config_.headless) {
    renderer_.UploadMesh(wall);
    renderer_.UploadMesh(wall_flat);
    renderer_.UploadMesh(floor_mesh);
  }
  ecs::Entity w0 = world_.Create();
  world_.Add(w0, scene::Transform{.position = {-3.2f, 2.0f, -2.0f}});
  world_.Add(w0, scene::Renderable{wall.id});
  ecs::Entity w1 = world_.Create();
  world_.Add(w1, scene::Transform{.position = {3.2f, 2.0f, -2.0f}});
  world_.Add(w1, scene::Renderable{wall_flat.id});
  ecs::Entity fl = world_.Create();
  world_.Add(fl, scene::Transform{.position = {0, -0.15f, 0}});
  world_.Add(fl, scene::Renderable{floor_mesh.id});

  // Decal atlas: left half a dried blood splat, right half a moss patch.
  asset::Texture atlas;
  atlas.id = asset::MakeAssetId("builtin/decals/atlas");
  atlas.format = asset::TextureFormat::kRgba8;
  atlas.width = 512;
  atlas.height = 256;
  atlas.is_srgb = true;
  atlas.data.resize(static_cast<size_t>(atlas.width) * atlas.height * 4);
  auto blob = [](f32 u, f32 v, u32 seed_base, int arms) {
    // Irregular radial blob: radius modulated by a few sine lobes.
    f32 du = u - 0.5f, dv = v - 0.5f;
    f32 r = std::sqrt(du * du + dv * dv) * 2.2f;
    f32 ang = std::atan2(dv, du);
    f32 rim = 0.75f + 0.18f * std::sin(ang * arms + seed_base) +
              0.12f * std::sin(ang * (arms * 2 + 1) + seed_base * 1.7f);
    return std::max(0.0f, 1.0f - r / rim);
  };
  for (u32 y = 0; y < atlas.height; ++y) {
    for (u32 x = 0; x < atlas.width; ++x) {
      size_t o = (static_cast<size_t>(y) * atlas.width + x) * 4;
      f32 v = (y + 0.5f) / atlas.height;
      if (x < 256) {  // blood
        f32 u = (x + 0.5f) / 256.0f;
        f32 m = blob(u, v, 3, 7);
        f32 a = m > 0.02f ? std::min(1.0f, m * 2.2f) : 0.0f;
        atlas.data[o] = static_cast<u8>(90 + 40 * m);
        atlas.data[o + 1] = static_cast<u8>(8 + 10 * m);
        atlas.data[o + 2] = static_cast<u8>(8 + 8 * m);
        atlas.data[o + 3] = static_cast<u8>(a * 255.0f);
      } else {  // moss
        f32 u = (x - 256 + 0.5f) / 256.0f;
        f32 m = blob(u, v, 11, 9);
        f32 grain = 0.7f + 0.3f * blob(std::fmod(u * 5.0f, 1.0f), std::fmod(v * 5.0f, 1.0f), 5, 5);
        f32 a = m > 0.05f ? std::min(1.0f, m * 1.6f) * grain : 0.0f;
        atlas.data[o] = static_cast<u8>(40 + 25 * m);
        atlas.data[o + 1] = static_cast<u8>(85 + 60 * m * grain);
        atlas.data[o + 2] = static_cast<u8>(28 + 15 * m);
        atlas.data[o + 3] = static_cast<u8>(a * 255.0f);
      }
    }
  }
  // Channel atlas: matching normal (decal-box space) page. Blood stays flat
  // (its story is the wet-glossy roughness override); moss gets bumpy
  // clump normals from the height of the same grain function.
  asset::Texture channels;
  channels.id = asset::MakeAssetId("builtin/decals/atlas_normal");
  channels.format = asset::TextureFormat::kRgba8;
  channels.width = 512;
  channels.height = 256;
  channels.is_srgb = false;  // normals are linear data
  channels.data.resize(static_cast<size_t>(channels.width) * channels.height * 4);
  auto moss_height = [&](f32 u, f32 v) {
    f32 m = blob(u, v, 11, 9);
    f32 grain = 0.7f + 0.3f * blob(std::fmod(u * 5.0f, 1.0f), std::fmod(v * 5.0f, 1.0f), 5, 5);
    return m * grain;
  };
  for (u32 y = 0; y < channels.height; ++y) {
    for (u32 x = 0; x < channels.width; ++x) {
      size_t o = (static_cast<size_t>(y) * channels.width + x) * 4;
      f32 nx = 0.0f, ny = 0.0f;
      if (x >= 256) {  // moss: finite-difference the clump height
        f32 u = (x - 256 + 0.5f) / 256.0f;
        f32 v = (y + 0.5f) / channels.height;
        const f32 e = 1.0f / 256.0f;
        nx = (moss_height(u - e, v) - moss_height(u + e, v)) * 3.0f;
        ny = (moss_height(u, v - e) - moss_height(u, v + e)) * 3.0f;
      }
      f32 nz = std::sqrt(std::max(1.0f - nx * nx - ny * ny, 0.05f));
      channels.data[o] = static_cast<u8>((nx * 0.5f + 0.5f) * 255.0f);
      channels.data[o + 1] = static_cast<u8>((ny * 0.5f + 0.5f) * 255.0f);
      channels.data[o + 2] = static_cast<u8>((nz * 0.5f + 0.5f) * 255.0f);
      channels.data[o + 3] = 255;
    }
  }
  if (!config_.headless) {
    renderer_.UploadTexture(atlas);
    renderer_.UploadTexture(channels);
    renderer_.SetDecalAtlas(atlas.id, channels.id);
  }
  // A blood splat + moss patches projected onto the pom wall and the floor.
  auto make_decal = [](Vec3 pos, Vec3 normal, Vec3 up_hint, f32 w, f32 h, f32 depth,
                       bool moss) {
    render::Decal d;
    Vec3 n = Normalize(normal);
    Vec3 t = Normalize(Cross(up_hint, n));
    Vec3 b = Cross(n, t);
    auto row = [&](Vec3 axis, f32 extent, f32* out) {
      out[0] = axis.x / extent;
      out[1] = axis.y / extent;
      out[2] = axis.z / extent;
      out[3] = -(axis.x * pos.x + axis.y * pos.y + axis.z * pos.z) / extent;
    };
    row(t, w, d.row0);
    row(b, h, d.row1);
    row(n, depth, d.row2);
    d.uv_rect[0] = 0.5f;
    d.uv_rect[1] = 1.0f;
    d.uv_rect[2] = moss ? 0.5f : 0.0f;
    d.uv_rect[3] = 0.0f;
    if (moss) {
      d.params2[0] = 0.85f;  // bumpy clumps
      d.params2[1] = 1.3f;   // rougher than the bricks
    } else {
      d.params2[0] = 0.0f;
      d.params2[1] = 0.22f;  // wet blood: glossy
    }
    return d;
  };
  demo_decals_.push_back(
      make_decal({-3.6f, 1.6f, -1.85f}, {0, 0, 1}, {0, 1, 0}, 0.7f, 0.7f, 0.4f, false));
  demo_decals_.push_back(
      make_decal({-2.2f, 0.8f, -1.85f}, {0, 0, 1}, {0, 1, 0}, 1.1f, 1.1f, 0.4f, true));
  demo_decals_.push_back(
      make_decal({-1.0f, 0.0f, 0.6f}, {0, 1, 0}, {0, 0, 1}, 0.9f, 0.9f, 0.3f, false));
  demo_decals_.push_back(
      make_decal({1.6f, 0.0f, 1.4f}, {0, 1, 0}, {0, 0, 1}, 1.3f, 1.3f, 0.3f, true));

  // Grazing warm sun to pop the relief.
  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = {-0.85f, -0.18f, -0.49f};
  renderer_.settings().sun_intensity = 3.0f;
  renderer_.settings().sun_color = {1.0f, 0.85f, 0.7f};

  camera_.set_position({-0.4f, 1.7f, 2.6f});
  camera_.set_yaw_pitch(-0.25f, -0.10f);
  camera_.speed = 3.0f;
  RX_INFO("brick demo: pom wall (left) vs flat normal-mapped wall (right)");
}

void DemoScenes::CreateSilhouettePomDemoScene() {
  // Silhouette-POM A/B: two identical studded spheres against the sky. The left
  // one carries the silhouette-aware flag - its outline is carved by the height
  // field (studs poke past the mesh edge, the gaps between them are eaten in),
  // so the limb reads as real relief. The right one runs classic POM: the same
  // interior displacement but a polygon-smooth circular outline. The carving is
  // strongest along the limb, where the view ray grazes the convex surface.
  constexpr u32 kTex = 256;
  constexpr f32 kTilesU = 10.0f, kTilesV = 5.0f;  // ~square studs over the 2:1 uv
  auto stud_height = [](f32 u, f32 v) -> f32 {
    // Grid of rounded domes; deep flat gaps between them give the limb something
    // to carve. Round-shouldered so the normal map stays smooth.
    f32 cu = u * kTilesU, cv = v * kTilesV;
    f32 fu = cu - std::floor(cu) - 0.5f;
    f32 fv = cv - std::floor(cv) - 0.5f;
    f32 d = std::sqrt(fu * fu + fv * fv) * 2.0f;  // 0 centre .. ~1.4 corner
    f32 h = std::max(0.0f, 1.0f - d / 0.82f);
    return h * h * (3.0f - 2.0f * h);  // smoothstep dome
  };
  asset::Texture height;
  height.id = asset::MakeAssetId("builtin/silpom/height");
  height.format = asset::TextureFormat::kRgba8;
  height.width = height.height = kTex;
  height.data.resize(static_cast<size_t>(kTex) * kTex * 4);
  asset::Texture albedo = height;
  albedo.id = asset::MakeAssetId("builtin/silpom/albedo");
  albedo.is_srgb = true;
  asset::Texture normal = height;
  normal.id = asset::MakeAssetId("builtin/silpom/normal");
  for (u32 y = 0; y < kTex; ++y) {
    for (u32 x = 0; x < kTex; ++x) {
      f32 u = (x + 0.5f) / kTex, v = (y + 0.5f) / kTex;
      f32 h = stud_height(u, v);
      size_t o = (static_cast<size_t>(y) * kTex + x) * 4;
      height.data[o] = height.data[o + 1] = height.data[o + 2] = static_cast<u8>(h * 255.0f);
      height.data[o + 3] = 255;
      f32 e = 1.0f / kTex;
      f32 hx = stud_height(u + e, v) - stud_height(u - e, v);
      f32 hy = stud_height(u, v + e) - stud_height(u, v - e);
      f32 nx = -hx * 6.0f, ny = -hy * 6.0f, nz = 1.0f;
      f32 len = std::sqrt(nx * nx + ny * ny + nz * nz);
      normal.data[o] = static_cast<u8>((nx / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 1] = static_cast<u8>((ny / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 2] = static_cast<u8>((nz / len * 0.5f + 0.5f) * 255.0f);
      normal.data[o + 3] = 255;
      // Warm sandstone studs, dark recessed gaps.
      f32 r = h * 0.72f + (1.0f - h) * 0.10f;
      f32 g = h * 0.52f + (1.0f - h) * 0.09f;
      f32 b = h * 0.34f + (1.0f - h) * 0.08f;
      albedo.data[o] = static_cast<u8>(r * 255.0f);
      albedo.data[o + 1] = static_cast<u8>(g * 255.0f);
      albedo.data[o + 2] = static_cast<u8>(b * 255.0f);
      albedo.data[o + 3] = 255;
    }
  }
  if (!config_.headless) {
    renderer_.UploadTexture(height);
    renderer_.UploadTexture(albedo);
    renderer_.UploadTexture(normal);
  }

  asset::Material sil;
  sil.id = asset::MakeAssetId("builtin/silpom/mat_sil");
  sil.base_color = albedo.id;
  sil.normal = normal.id;
  sil.height = height.id;
  sil.height_scale = 0.09f;
  sil.roughness_factor = 0.85f;
  sil.silhouette_pom = true;
  sil.silhouette_curvature = 0.5f;
  asset::Material classic = sil;  // same look, polygon-straight outline
  classic.id = asset::MakeAssetId("builtin/silpom/mat_classic");
  classic.silhouette_pom = false;
  if (!config_.headless) {
    renderer_.UploadMaterial(sil);
    renderer_.UploadMaterial(classic);
  }

  auto spawn = [&](const char* tag, Vec3 pos, const asset::Material& mat) {
    asset::Mesh sphere = asset::MakeSphere(0.7f, 48, 64, asset::MakeAssetId(tag));
    sphere.lods[0].submeshes[0].material = mat.id;
    if (!config_.headless) renderer_.UploadMesh(sphere);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{sphere.id});
  };
  spawn("builtin/silpom/sphere_sil", {-0.95f, 1.3f, 0.0f}, sil);
  spawn("builtin/silpom/sphere_classic", {0.95f, 1.3f, 0.0f}, classic);

  // Grazing warm sun to rake the studs; spheres float well above a distant floor
  // so their upper limbs read against the sky where the carve is clearest.
  asset::Mesh floor_mesh =
      asset::MakeBox(24.0f, 0.2f, 24.0f, asset::MakeAssetId("builtin/silpom/floor"));
  if (!config_.headless) renderer_.UploadMesh(floor_mesh);
  ecs::Entity fl = world_.Create();
  world_.Add(fl, scene::Transform{.position = {0, -1.6f, 0}});
  world_.Add(fl, scene::Renderable{floor_mesh.id});

  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = {-0.82f, -0.22f, -0.52f};
  renderer_.settings().sun_intensity = 3.2f;
  renderer_.settings().sun_color = {1.0f, 0.86f, 0.72f};

  camera_.set_position({0.0f, 1.35f, 3.15f});
  camera_.set_yaw_pitch(0.0f, -0.02f);
  camera_.speed = 2.5f;
  RX_INFO("silhouette pom: left sphere carves its outline, right sphere is classic flat-edge pom");
}

void DemoScenes::CreateSssDemoScene() {
  // Screen-space subsurface scattering A/B: two identical skin-toned spheres
  // under a hard side sun. The right one carries the skin flag (diffuse routed
  // through the sss blur - soft terminator, red bleed); the left is the
  // control. Both share the analytic subsurface term so the only difference
  // is the screen-space diffusion.
  asset::Material floor_mat;
  floor_mat.id = asset::MakeAssetId("builtin/sss/floor");
  floor_mat.base_color_factor[0] = 0.30f;
  floor_mat.base_color_factor[1] = 0.30f;
  floor_mat.base_color_factor[2] = 0.32f;
  floor_mat.roughness_factor = 0.9f;
  if (!config_.headless) renderer_.UploadMaterial(floor_mat);
  asset::Mesh ground =
      asset::MakeBox(8.0f, 0.15f, 6.0f, asset::MakeAssetId("builtin/sss/ground"));
  ground.lods[0].submeshes.push_back(
      {0, static_cast<u32>(ground.lods[0].indices.size()), floor_mat.id});
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -0.15f, 0}});
  world_.Add(floor, scene::Renderable{ground.id});

  auto spawn_sphere = [&](Vec3 pos, f32 radius, bool skin, f32 perfusion, const char* tag) {
    asset::Material mat;
    mat.id = asset::MakeAssetId(std::string("builtin/sss/mat_") + tag);
    mat.base_color_factor[0] = 0.62f;
    mat.base_color_factor[1] = 0.44f;
    mat.base_color_factor[2] = 0.34f;
    mat.roughness_factor = 0.55f;
    mat.subsurface = 0.20f;
    mat.subsurface_color[0] = 0.9f;
    mat.subsurface_color[1] = 0.25f;
    mat.subsurface_color[2] = 0.15f;
    mat.skin = skin;
    // Physical skin scattering + blood flow (0.5 rest, higher = flushed).
    mat.skin_params.scatter_color[0] = 0.85f;
    mat.skin_params.scatter_color[1] = 0.55f;
    mat.skin_params.scatter_color[2] = 0.40f;
    mat.skin_params.mfp[0] = 1.0f;
    mat.skin_params.mfp[1] = 0.35f;
    mat.skin_params.mfp[2] = 0.20f;
    mat.skin_params.perfusion = perfusion;
    asset::Mesh sphere =
        asset::MakeSphere(radius, 48, 64, asset::MakeAssetId(std::string("builtin/sss/") + tag));
    sphere.lods[0].submeshes[0].material = mat.id;
    if (!config_.headless) {
      renderer_.UploadMaterial(mat);
      renderer_.UploadMesh(sphere);
    }
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{sphere.id});
  };
  spawn_sphere({-0.75f, 0.6f, 0.0f}, 0.55f, false, 0.5f, "control");
  spawn_sphere({0.75f, 0.6f, 0.0f}, 0.55f, true, 0.5f, "skin");
  // A small pair further back: the blur radius must shrink with distance.
  spawn_sphere({-0.35f, 0.25f, -1.6f}, 0.22f, false, 0.5f, "control_far");
  spawn_sphere({0.35f, 0.25f, -1.6f}, 0.22f, true, 0.5f, "skin_far");
  // Blood-flow A/B: flushed (high perfusion) vs blanched (low).
  spawn_sphere({-1.35f, 0.35f, -0.8f}, 0.28f, true, 0.9f, "skin_flush");
  spawn_sphere({1.35f, 0.35f, -0.8f}, 0.28f, true, 0.1f, "skin_blanch");

  // Hair pair on the flanks: dark spheres whose latitude tangents act as
  // strands. The right one carries the hair flag (dual anisotropic bands);
  // the left is the plain ggx control blob.
  auto spawn_hair = [&](Vec3 pos, bool hair, const char* tag) {
    asset::Material mat;
    mat.id = asset::MakeAssetId(std::string("builtin/sss/hairmat_") + tag);
    mat.base_color_factor[0] = 0.16f;
    mat.base_color_factor[1] = 0.10f;
    mat.base_color_factor[2] = 0.06f;
    mat.roughness_factor = 0.45f;
    mat.hair = hair;
    asset::Mesh sphere = asset::MakeSphere(
        0.45f, 48, 64, asset::MakeAssetId(std::string("builtin/sss/hair_") + tag));
    sphere.lods[0].submeshes[0].material = mat.id;
    if (!config_.headless) {
      renderer_.UploadMaterial(mat);
      renderer_.UploadMesh(sphere);
    }
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{sphere.id});
  };
  spawn_hair({-2.05f, 0.5f, -0.35f}, false, "control");
  spawn_hair({2.05f, 0.5f, -0.35f}, true, "aniso");

  // A slim pole per sphere, placed sunward so its hard shadow line crosses the
  // camera-facing hemisphere: that edge is where the diffusion reads
  // (softening + red bleed). Smoothly lit spheres alone would show nothing -
  // a gaussian preserves linear ramps.
  asset::Mesh pole =
      asset::MakeBox(0.035f, 0.7f, 0.035f, asset::MakeAssetId("builtin/sss/pole"));
  pole.lods[0].submeshes.push_back(
      {0, static_cast<u32>(pole.lods[0].indices.size()), floor_mat.id});
  if (!config_.headless) renderer_.UploadMesh(pole);
  const f32 poles[2][3] = {{1.30f, 0.95f, 0.75f}, {-0.20f, 0.95f, 0.75f}};
  for (auto& pp : poles) {
    ecs::Entity occluder = world_.Create();
    world_.Add(occluder, scene::Transform{.position = {pp[0], pp[1], pp[2]}});
    world_.Add(occluder, scene::Renderable{pole.id});
  }

  // Hard side sun so the terminator crosses both spheres mid-face. DoF off:
  // its near-field blur would mask the effect this demo exists to show.
  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = {-0.90f, -0.30f, -0.32f};
  renderer_.settings().sun_intensity = 4.5f;
  renderer_.settings().sun_color = {1.0f, 0.94f, 0.88f};
  renderer_.settings().dof = false;

  camera_.set_position({0.0f, 0.72f, 2.0f});
  camera_.set_yaw_pitch(0.0f, -0.04f);
  camera_.speed = 2.0f;
  RX_INFO("sss demo: skin sphere (right) vs control (left)");
}

void DemoScenes::CreateFireDemoScene() {
  // A campfire at dusk: stone ground, a log ring, gpu-simulated flames and
  // embers (additive hdr), and a flickering point light that the rt path
  // shadows. Exercises the whole fire stack in one shot.
  asset::Material stone;
  stone.id = asset::MakeAssetId("builtin/fire/stone");
  stone.base_color_factor[0] = 0.23f;
  stone.base_color_factor[1] = 0.22f;
  stone.base_color_factor[2] = 0.21f;
  stone.roughness_factor = 0.9f;
  if (!config_.headless) renderer_.UploadMaterial(stone);
  asset::Material wood;
  wood.id = asset::MakeAssetId("builtin/fire/wood");
  wood.base_color_factor[0] = 0.16f;
  wood.base_color_factor[1] = 0.09f;
  wood.base_color_factor[2] = 0.05f;
  wood.roughness_factor = 0.85f;
  if (!config_.headless) renderer_.UploadMaterial(wood);

  asset::Mesh ground = asset::MakeBox(30.0f, 0.2f, 30.0f, asset::MakeAssetId("builtin/fire/ground"));
  ground.lods[0].submeshes.push_back(
      {0, static_cast<u32>(ground.lods[0].indices.size()), stone.id});
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -0.2f, 0}});
  world_.Add(floor, scene::Renderable{ground.id});

  // Log ring: slim boxes leaned into the fire, plus a few seat rocks so the
  // flicker light has geometry to shadow.
  asset::Mesh log = asset::MakeBox(0.9f, 0.14f, 0.14f, asset::MakeAssetId("builtin/fire/log"));
  log.lods[0].submeshes.push_back({0, static_cast<u32>(log.lods[0].indices.size()), wood.id});
  if (!config_.headless) renderer_.UploadMesh(log);
  for (int i = 0; i < 5; ++i) {
    f32 a = static_cast<f32>(i) * 1.2566f;
    Quat q = QuatFromAxisAngle({0, 1, 0}, a);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {std::cos(a) * 0.28f, 0.10f + 0.02f * i,
                                                std::sin(a) * 0.28f},
                                   .rotation = {q.x, q.y, q.z, q.w}});
    world_.Add(e, scene::Renderable{log.id});
  }
  asset::Mesh rock = asset::MakeSphere(0.45f, 16, 24, asset::MakeAssetId("builtin/fire/rock"));
  rock.lods[0].submeshes.push_back({0, static_cast<u32>(rock.lods[0].indices.size()), stone.id});
  if (!config_.headless) renderer_.UploadMesh(rock);
  const f32 rocks[4][2] = {{2.2f, 0.6f}, {-1.8f, 1.4f}, {0.6f, -2.1f}, {-2.4f, -1.2f}};
  for (auto& r : rocks) {
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {r[0], 0.25f, r[1]}});
    world_.Add(e, scene::Renderable{rock.id});
  }

  // Two wind-swayed banners flanking the fire (the cloth material carries the
  // wind flag; uv.y = 0 at the pinned top edge per the sway convention).
  asset::Material cloth;
  cloth.id = asset::MakeAssetId("builtin/fire/cloth");
  cloth.base_color_factor[0] = 0.55f;
  cloth.base_color_factor[1] = 0.12f;
  cloth.base_color_factor[2] = 0.10f;
  cloth.roughness_factor = 0.85f;
  cloth.two_sided = true;
  cloth.wind = true;
  cloth.sheen_color[0] = cloth.sheen_color[1] = cloth.sheen_color[2] = 0.35f;
  cloth.sheen_roughness = 0.5f;
  if (!config_.headless) renderer_.UploadMaterial(cloth);

  asset::Mesh banner;
  banner.id = asset::MakeAssetId("builtin/fire/banner");
  banner.lods.resize(1);
  {
    constexpr int kW = 8, kH = 12;  // subdivided so the sway bends smoothly
    const f32 width = 0.9f, height = 1.6f;
    asset::MeshLod& lod = banner.lods[0];
    for (int y = 0; y <= kH; ++y) {
      for (int x = 0; x <= kW; ++x) {
        asset::Vertex v{};
        f32 u = static_cast<f32>(x) / kW;
        f32 vv = static_cast<f32>(y) / kH;
        v.position[0] = (u - 0.5f) * width;
        v.position[1] = -vv * height;  // hangs downward from the rod
        v.position[2] = 0.0f;
        v.normal[2] = 1.0f;
        v.tangent[0] = 1.0f;
        v.tangent[3] = 1.0f;
        v.uv[0] = u;
        v.uv[1] = vv;  // 0 at the pinned edge
        lod.vertices.push_back(v);
      }
    }
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        u32 a = y * (kW + 1) + x;
        u32 b = a + 1;
        u32 c = a + (kW + 1);
        u32 d = c + 1;
        for (u32 idx : {a, c, b, b, c, d}) lod.indices.push_back(idx);
      }
    }
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), cloth.id});
  }
  if (!config_.headless) renderer_.UploadMesh(banner);
  asset::Mesh pole = asset::MakeBox(0.04f, 1.3f, 0.04f, asset::MakeAssetId("builtin/fire/pole"));
  pole.lods[0].submeshes.push_back({0, static_cast<u32>(pole.lods[0].indices.size()), wood.id});
  if (!config_.headless) renderer_.UploadMesh(pole);
  const f32 banners[2][2] = {{-2.6f, -0.8f}, {2.3f, -1.6f}};
  for (auto& b : banners) {
    ecs::Entity pe = world_.Create();
    world_.Add(pe, scene::Transform{.position = {b[0], 1.3f, b[1]}});
    world_.Add(pe, scene::Renderable{pole.id});
    ecs::Entity be = world_.Create();
    world_.Add(be, scene::Transform{.position = {b[0], 2.5f, b[1]}});
    world_.Add(be, scene::Renderable{banner.id});
  }

  gpu_particle_count_ = 3000;
  gpu_particle_emitter_ = {0.0f, 0.12f, 0.0f};
  gpu_particle_mode_ = 1;
  gpu_particle_radius_ = 0.26f;
  gpu_particle_intensity_ = 0.55f;

  // Dusk: the sun barely up so the fire owns the scene. Auto exposure would
  // brighten the dim scene back to noon; bias it down for the mood.
  ctx_.scene_owns_sun = true;
  renderer_.settings().sun_direction = {0.35f, -0.08f, -0.93f};
  renderer_.settings().sun_intensity = 0.25f;
  renderer_.settings().sun_color = {1.0f, 0.55f, 0.35f};
  renderer_.settings().ambient = 0.03f;
  renderer_.settings().cloud_coverage = 0.6f;
  renderer_.settings().exposure = 0.30f;

  camera_.set_position({2.6f, 1.5f, 3.4f});
  camera_.set_yaw_pitch(-0.65f, -0.22f);
  camera_.speed = 3.0f;
  RX_INFO("fire demo: {} gpu flame/ember particles + flickering shadowed light",
           gpu_particle_count_);
}

void DemoScenes::CreateFurDemoScene() {
  // A fuzzy sphere: an opaque brown core (so the shells have a solid base and a
  // depth occluder) under the shell-fur pass that draws the coat.
  asset::Mesh ground = asset::MakeCube(12.0f, asset::MakeAssetId("builtin/fur/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -12.0f, 0}});  // top at y = 0
  world_.Add(floor, scene::Renderable{ground.id});

  asset::Material core;
  core.id = asset::MakeAssetId("builtin/fur/core");
  core.base_color_factor[0] = 0.4f;
  core.base_color_factor[1] = 0.28f;
  core.base_color_factor[2] = 0.15f;
  core.roughness_factor = 0.9f;
  if (!config_.headless) renderer_.UploadMaterial(core);
  asset::Mesh sphere = asset::MakeSphere(1.0f, 64, 96, asset::MakeAssetId("builtin/fur/coremesh"));
  sphere.lods[0].submeshes[0].material = core.id;
  if (!config_.headless) renderer_.UploadMesh(sphere);
  ecs::Entity ball = world_.Create();
  world_.Add(ball, scene::Transform{.position = {0, 1.05f, 0}});
  world_.Add(ball, scene::Renderable{sphere.id});

  fur_ball_ = true;
  fur_position_ = {0.0f, 1.05f, 0.0f};

  camera_.set_position({0.0f, 1.4f, 4.2f});
  camera_.set_yaw_pitch(0.0f, -0.06f);
  camera_.speed = 3.0f;
  RX_INFO("fur demo: shell-based hair/fur on a sphere");
}

void DemoScenes::CreateAutoLodDemoScene() {
  // One high-poly sphere (~19k tris, a single authored lod) whose coarser lods
  // are generated by the mesh simplifier, then instanced down a receding line so
  // the gpu picks the decimated lods with distance.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/autolod/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -8.1f, 0}});  // top at y = -0.1
  world_.Add(floor, scene::Renderable{ground.id});

  asset::Material mat;
  mat.id = asset::MakeAssetId("builtin/autolod/mat");
  mat.base_color_factor[0] = 0.3f;
  mat.base_color_factor[1] = 0.55f;
  mat.base_color_factor[2] = 0.85f;
  mat.roughness_factor = 0.4f;
  if (!config_.headless) renderer_.UploadMaterial(mat);

  asset::Mesh sphere = asset::MakeSphere(1.0f, 80, 120, asset::MakeAssetId("builtin/autolod/mesh"));
  sphere.lods[0].submeshes[0].material = mat.id;
  asset::GenerateLods(&sphere);  // appends the decimated lods
  for (size_t i = 0; i < sphere.lods.size(); ++i) {
    RX_INFO("auto-lod: lod{} = {} tris", i, sphere.lods[i].indices.size() / 3);
  }
  if (!config_.headless) renderer_.UploadMesh(sphere);

  // One mesh, instanced at increasing distance: each instance picks its lod.
  const Vec3 pos[3] = {{-1.7f, 0.9f, 5.0f}, {1.5f, 0.9f, 3.0f}, {-1.0f, 0.9f, 1.0f}};
  for (const Vec3& p : pos) {
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {p.x, p.y, p.z}});
    world_.Add(e, scene::Renderable{sphere.id});
  }

  camera_.set_position({0.0f, 1.5f, 6.5f});
  camera_.set_yaw_pitch(0.0f, -0.1f);
  camera_.speed = 4.0f;
  RX_INFO("auto-lod demo: decimated lods on a single high-poly sphere");
}

void DemoScenes::CreateOitDemoScene() {
  // Five interpenetrating transparent spheres of different colours; weighted
  // blended oit composites them with no sorting, so every layer shows through.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/oit/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -8.0f, 0}});  // top at y = 0
  world_.Add(floor, scene::Renderable{ground.id});

  struct S {
    Vec3 pos;
    Vec3 color;
    f32 alpha;
  };
  const S spheres[5] = {{{-0.45f, 1.2f, 0.25f}, {1.0f, 0.12f, 0.12f}, 0.55f},
                        {{0.45f, 1.2f, -0.2f}, {0.12f, 1.0f, 0.18f}, 0.55f},
                        {{0.0f, 1.2f, 0.5f}, {0.15f, 0.3f, 1.0f}, 0.55f},
                        {{0.0f, 1.65f, 0.0f}, {1.0f, 0.95f, 0.15f}, 0.5f},
                        {{0.0f, 0.75f, 0.1f}, {0.15f, 1.0f, 1.0f}, 0.5f}};
  const f32 radius = 0.6f;
  bool reverse = bool(OitReverse);  // verify order independence
  for (int j = 0; j < 5; ++j) {
    const S& s = spheres[reverse ? 4 - j : j];
    render::WboitInstance inst;
    Mat4 m{};
    m.m[0] = m.m[5] = m.m[10] = radius;
    m.m[15] = 1.0f;
    m.m[12] = s.pos.x;
    m.m[13] = s.pos.y;
    m.m[14] = s.pos.z;
    inst.model = m;
    inst.color[0] = s.color.x;
    inst.color[1] = s.color.y;
    inst.color[2] = s.color.z;
    inst.color[3] = s.alpha;
    oit_instances_.push_back(inst);
  }

  camera_.set_position({0.0f, 1.3f, 4.2f});
  camera_.set_yaw_pitch(0.0f, -0.06f);
  camera_.speed = 3.0f;
  RX_INFO("oit demo: {} overlapping transparent spheres{}", oit_instances_.size(),
           reverse ? " (reversed order)" : "");
}

void DemoScenes::CreateOcclusionDemoScene() {
  // A large wall directly in front of the camera hides a dense grid of small
  // cubes behind it. The gpu hi-z pass culls the hidden cubes against last
  // frame's depth, so the visible-draw count drops to roughly the wall + floor
  // even though every cube is still submitted. Strafe sideways and the cubes
  // reappear as they leave the wall's shadow. Verified via the debug overlay
  // ("opaque draws: N / M visible") and RX_NO_OCCLUSION for the A/B baseline.
  auto mat = [&](const char* tag, f32 r, f32 g, f32 b) {
    asset::Material m;
    m.id = asset::MakeAssetId(tag);
    m.base_color_factor[0] = r;
    m.base_color_factor[1] = g;
    m.base_color_factor[2] = b;
    m.roughness_factor = 0.6f;
    m.metallic_factor = 0.0f;
    if (!config_.headless) renderer_.UploadMaterial(m);
    return m.id;
  };
  asset::AssetId floor_mat = mat("builtin/occl/floor", 0.5f, 0.5f, 0.55f);
  asset::AssetId wall_mat = mat("builtin/occl/wall", 0.7f, 0.3f, 0.2f);
  asset::AssetId cube_mat = mat("builtin/occl/cube", 0.2f, 0.6f, 0.8f);

  auto add_box = [&](asset::Mesh mesh, asset::AssetId material, Vec3 pos) {
    asset::MeshLod& lod = mesh.lods[0];  // MakeBox leaves the submesh list empty
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{mesh.id});
  };

  add_box(asset::MakeBox(8.0f, 0.1f, 8.0f, asset::MakeAssetId("builtin/occl/ground")), floor_mat,
          {0, -0.1f, 0});
  // The occluder: tall and wide enough to fully cover the cube grid's silhouette.
  add_box(asset::MakeBox(3.0f, 2.2f, 0.1f, asset::MakeAssetId("builtin/occl/wall")), wall_mat,
          {0, 1.6f, 2.0f});

  // Grid of small cubes (half-extent 0.04, so the screen footprint stays inside
  // the coarse hi-z's small-object window) clustered behind the wall.
  int idx = 0;
  for (int gx = 0; gx < 12; ++gx) {
    for (int gy = 0; gy < 10; ++gy) {
      f32 x = -1.1f + gx * 0.2f;
      f32 y = 0.7f + gy * 0.2f;
      f32 z = -2.0f - (gx % 3) * 0.6f;
      std::string tag = "builtin/occl/c" + std::to_string(idx++);
      add_box(asset::MakeCube(0.04f, asset::MakeAssetId(tag)), cube_mat, {x, y, z});
    }
  }

  camera_.set_position({0.0f, 1.6f, 6.0f});
  camera_.set_yaw_pitch(0.0f, 0.0f);
  camera_.speed = 3.0f;
  RX_INFO("occlusion demo: {} small cubes hidden behind a wall (gpu hi-z cull)", idx);
}

void DemoScenes::CreatePointLightDemoScene() {
  // A grid of white tiles under a row of colored omni lights, with the sun dimmed
  // so the dynamic point lights dominate. Verifies forward point lighting and the
  // light-complexity view (RX_DEBUG_VIEW=15) where light volumes overlap.
  asset::Material floor_mat;
  floor_mat.id = asset::MakeAssetId("builtin/lights/floor");
  floor_mat.base_color_factor[0] = floor_mat.base_color_factor[1] = floor_mat.base_color_factor[2] =
      0.32f;  // mid-dark so colored light reflects as colour, not blown-out white
  floor_mat.roughness_factor = 0.5f;
  floor_mat.metallic_factor = 0.0f;
  if (!config_.headless) renderer_.UploadMaterial(floor_mat);

  asset::Mesh ground = asset::MakeBox(6.0f, 0.1f, 6.0f, asset::MakeAssetId("builtin/lights/ground"));
  ground.lods[0].submeshes.push_back(
      {0, static_cast<u32>(ground.lods[0].indices.size()), floor_mat.id});
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -0.1f, 0}});
  world_.Add(floor, scene::Renderable{ground.id});

  // A few low bumps so the lights wrap over shapes, not just a flat plane.
  for (int i = 0; i < 5; ++i) {
    f32 x = -3.2f + i * 1.6f;
    std::string tag = "builtin/lights/bump" + std::to_string(i);
    asset::Mesh s = asset::MakeSphere(0.6f, 24, 32, asset::MakeAssetId(tag));
    s.lods[0].submeshes.push_back({0, static_cast<u32>(s.lods[0].indices.size()), floor_mat.id});
    if (!config_.headless) renderer_.UploadMesh(s);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {x, 0.3f, 0.0f}});
    world_.Add(e, scene::Renderable{s.id});
  }

  const f32 col[8][3] = {{1, 0.2f, 0.2f}, {0.2f, 1, 0.2f}, {0.3f, 0.4f, 1}, {1, 0.9f, 0.2f},
                         {1, 0.3f, 1},    {0.2f, 1, 1},    {1, 0.6f, 0.2f}, {0.6f, 0.4f, 1}};
  for (int i = 0; i < 8; ++i) {
    render::PointLight l;
    l.pos_radius[0] = -3.5f + i * 1.0f;
    l.pos_radius[1] = 0.9f;
    l.pos_radius[2] = -0.5f + (i % 2) * 1.0f;
    l.pos_radius[3] = 2.2f;  // influence radius (overlapping, for the complexity view)
    l.color_intensity[0] = col[i][0];
    l.color_intensity[1] = col[i][1];
    l.color_intensity[2] = col[i][2];
    l.color_intensity[3] = 4.0f;
    demo_lights_.push_back(l);
  }

  // New light types: a spot cone sweeping the left floor, a warm sphere-area
  // lamp, and a cool rect panel washing the right wall of tiles.
  {
    render::PointLight spot;
    spot.pos_radius[0] = -4.5f; spot.pos_radius[1] = 3.0f; spot.pos_radius[2] = 2.5f;
    spot.pos_radius[3] = 9.0f;
    spot.color_intensity[0] = 1.0f; spot.color_intensity[1] = 0.95f;
    spot.color_intensity[2] = 0.8f; spot.color_intensity[3] = 20.0f;
    f32 dir[3] = {0.35f, -0.85f, -0.4f};
    f32 len = std::sqrt(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
    spot.direction_type[0] = dir[0]/len; spot.direction_type[1] = dir[1]/len;
    spot.direction_type[2] = dir[2]/len; spot.direction_type[3] = 1.0f;  // spot
    spot.params[0] = std::cos(0.28f);  // inner ~16 deg
    spot.params[1] = std::cos(0.45f);  // outer ~26 deg
    demo_lights_.push_back(spot);

    render::PointLight ball;
    ball.pos_radius[0] = 4.6f; ball.pos_radius[1] = 0.7f; ball.pos_radius[2] = 2.2f;
    ball.pos_radius[3] = 6.0f;
    ball.color_intensity[0] = 1.0f; ball.color_intensity[1] = 0.6f;
    ball.color_intensity[2] = 0.25f; ball.color_intensity[3] = 8.0f;
    ball.direction_type[3] = 2.0f;  // sphere area
    ball.params[0] = 0.35f;         // source radius
    demo_lights_.push_back(ball);

    render::PointLight panel;
    panel.pos_radius[0] = 0.0f; panel.pos_radius[1] = 1.6f; panel.pos_radius[2] = -3.2f;
    panel.pos_radius[3] = 8.0f;
    panel.color_intensity[0] = 0.4f; panel.color_intensity[1] = 0.7f;
    panel.color_intensity[2] = 1.0f; panel.color_intensity[3] = 6.0f;
    panel.direction_type[0] = 0.0f; panel.direction_type[1] = 0.0f;
    panel.direction_type[2] = 1.0f; panel.direction_type[3] = 3.0f;  // rect area
    panel.params[0] = 1.4f;  // half width
    panel.params[1] = 0.8f;  // half height
    demo_lights_.push_back(panel);
  }

  // Occluder pillars between lights and floor so the local shadow maps have
  // something to block (verifies the atlas on the raster tier).
  asset::Mesh pillar =
      asset::MakeBox(0.18f, 1.1f, 0.55f, asset::MakeAssetId("builtin/lights/pillar"));
  pillar.lods[0].submeshes.push_back(
      {0, static_cast<u32>(pillar.lods[0].indices.size()), floor_mat.id});
  if (!config_.headless) renderer_.UploadMesh(pillar);
  const f32 pillars[3][2] = {{-2.0f, 0.45f}, {0.4f, 0.5f}, {2.6f, 0.4f}};
  for (auto& pp : pillars) {
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pp[0], 0.55f, pp[1]}});
    world_.Add(e, scene::Renderable{pillar.id});
  }

  ctx_.scene_owns_sun = true;  // keep the day/night clock off the staged dusk
  renderer_.settings().sun_intensity = 0.25f;  // dim the sun + ibl so the point lights dominate
  renderer_.settings().sun_direction = {0.2f, -0.25f, -0.95f};
  renderer_.settings().ibl = false;
  renderer_.settings().ambient = 0.02f;
  // Fixed exposure: auto exposure would lift the deliberately dark scene back
  // to daylight and wash the colored lights out.
  renderer_.settings().auto_exposure = false;
  renderer_.settings().exposure = 1.0f;
  renderer_.settings().dof = false;
  // Low grazing view toward the rect panel so its floor reflection streak
  // (the LTC signature - elongating with roughness) is in frame.
  camera_.set_position({0.0f, 1.1f, 4.8f});
  camera_.set_yaw_pitch(0.0f, -0.16f);
  camera_.speed = 3.0f;
  RX_INFO("point-light demo: {} dynamic omni lights", demo_lights_.size());
}

// Weather demo defaults, routed through the same envs the renderer honours so
// RX_PRECIP / RX_SNOW / RX_WIND / RX_WIND_DIR still override the staged storm.
static base::Option<float> WeatherDemoPrecip{"demo.weather.precip", 0.85f, "RX_PRECIP"};
static base::Option<bool> WeatherDemoSnow{"demo.weather.snow", false, "RX_SNOW"};
static base::Option<float> WeatherDemoWind{"demo.weather.wind", 7.0f, "RX_WIND"};
static base::Option<float> WeatherDemoWindDir{"demo.weather.winddir", 205.0f, "RX_WIND_DIR"};
// Deterministic strike hook for tests/screenshots: RX_STRIKE_TEST=<meters>
// fires a strike 1 s after start directly in front of the camera at that
// distance, repeating every 4 s. 0 = the random storm scheduler.
static base::Option<float> WeatherStrikeTest{"demo.weather.striketest", 0.0f, "RX_STRIKE_TEST"};

void DemoScenes::CreateWeatherDemoScene() {
  // The volumetric-precipitation acceptance scene: a ground plane, varied
  // boxes/columns, and an open-sided shelter (flat roof on posts) so the
  // sky-occlusion gating is legible - rain must pour beside the shelter and
  // visibly stop under its roof, splashes must land ON the roof, and a dry
  // strip must appear beneath it. Two point lights make night shots readable.
  auto mat = [&](const char* tag, f32 r, f32 g, f32 b, f32 rough) {
    asset::Material m;
    m.id = asset::MakeAssetId(tag);
    m.base_color_factor[0] = r;
    m.base_color_factor[1] = g;
    m.base_color_factor[2] = b;
    m.roughness_factor = rough;
    m.metallic_factor = 0.0f;
    if (!config_.headless) renderer_.UploadMaterial(m);
    return m.id;
  };
  // Asphalt-dark ground so wet darkening, puddle sheen and splashes read
  // against it; mid-tone varied blocks so the streak slant shows on walls.
  asset::AssetId ground_mat = mat("builtin/weather/ground", 0.21f, 0.21f, 0.23f, 0.9f);
  asset::AssetId block_mat = mat("builtin/weather/block", 0.45f, 0.38f, 0.32f, 0.7f);
  asset::AssetId roof_mat = mat("builtin/weather/roof", 0.24f, 0.26f, 0.3f, 0.6f);

  auto add_box = [&](asset::Mesh mesh, asset::AssetId material, Vec3 pos) {
    asset::MeshLod& lod = mesh.lods[0];  // MakeBox leaves the submesh list empty
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{mesh.id});
  };

  // Ground plane, top face at y = 0.
  add_box(asset::MakeBox(40.0f, 0.5f, 40.0f, asset::MakeAssetId("builtin/weather/ground_m")),
          ground_mat, {0, -0.5f, 0});

  // Varied blocks and columns: wet/snowy top faces at several heights, and
  // vertical faces the wind-slanted rain streaks read against.
  add_box(asset::MakeBox(1.5f, 1.5f, 1.5f, asset::MakeAssetId("builtin/weather/block_a")),
          block_mat, {4.0f, 1.5f, -1.0f});
  add_box(asset::MakeBox(0.8f, 0.8f, 0.8f, asset::MakeAssetId("builtin/weather/block_b")),
          block_mat, {1.6f, 0.8f, 2.6f});
  add_box(asset::MakeBox(1.0f, 2.6f, 1.0f, asset::MakeAssetId("builtin/weather/tower")),
          block_mat, {-1.5f, 2.6f, -7.0f});
  add_box(asset::MakeBox(0.35f, 2.0f, 0.35f, asset::MakeAssetId("builtin/weather/column_a")),
          block_mat, {7.5f, 2.0f, 4.0f});
  add_box(asset::MakeBox(0.35f, 1.4f, 0.35f, asset::MakeAssetId("builtin/weather/column_b")),
          block_mat, {-8.0f, 1.4f, 3.0f});

  // Open-sided shelter: flat roof on four thin posts. Everything under the
  // 8 x 6 m roof must stay dry.
  const Vec3 shelter{-4.0f, 0.0f, -1.0f};
  add_box(asset::MakeBox(4.0f, 0.15f, 3.0f, asset::MakeAssetId("builtin/weather/roof_m")),
          roof_mat, {shelter.x, 3.15f, shelter.z});
  asset::Mesh post = asset::MakeBox(0.12f, 1.5f, 0.12f, asset::MakeAssetId("builtin/weather/post"));
  post.lods[0].submeshes.push_back(
      {0, static_cast<u32>(post.lods[0].indices.size()), block_mat});
  if (!config_.headless) renderer_.UploadMesh(post);
  const f32 posts[4][2] = {{-3.6f, -2.6f}, {3.6f, -2.6f}, {-3.6f, 2.6f}, {3.6f, 2.6f}};
  for (auto& pp : posts) {
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {shelter.x + pp[0], 1.5f, shelter.z + pp[1]}});
    world_.Add(e, scene::Renderable{post.id});
  }

  // Two warm lamps for night shots: one under the shelter roof (dry pocket in
  // the rain), one out in the open where drops catch the light.
  render::PointLight lamp;
  lamp.pos_radius[0] = shelter.x;
  lamp.pos_radius[1] = 2.6f;
  lamp.pos_radius[2] = shelter.z;
  lamp.pos_radius[3] = 10.0f;
  lamp.color_intensity[0] = 1.0f;
  lamp.color_intensity[1] = 0.75f;
  lamp.color_intensity[2] = 0.45f;
  lamp.color_intensity[3] = 4.0f;
  demo_lights_.push_back(lamp);
  lamp.pos_radius[0] = 4.0f;
  lamp.pos_radius[1] = 3.6f;
  lamp.pos_radius[2] = 2.0f;
  lamp.pos_radius[3] = 9.0f;
  lamp.color_intensity[0] = 0.85f;
  lamp.color_intensity[1] = 0.9f;
  lamp.color_intensity[2] = 1.0f;
  lamp.color_intensity[3] = 4.5f;
  demo_lights_.push_back(lamp);

  // The staged storm (env-overridable, see the options above).
  render::RenderSettings& rs = renderer_.settings();
  rs.weather.precipitation = WeatherDemoPrecip.get();
  rs.weather.snow = WeatherDemoSnow;
  rs.weather.wind_speed = WeatherDemoWind.get();
  rs.weather.wind_yaw = WeatherDemoWindDir.get() * 3.14159265f / 180.0f;
  rs.weather.gustiness = 0.45f;
  rs.cloud_coverage = 0.68f;  // overcast enough to sell the downpour, some light through
  // Storm light: the cloud deck kills most direct sun, but the IBL/sun path
  // does not know about clouds - stage it. Fixed exposure keeps the deliberate
  // gloom (auto exposure would lift the overcast scene back to a bright noon).
  rs.sun_intensity = 1.3f;
  rs.ibl_intensity = 0.4f;  // the sky cubemap is clear-sky bright; mute it under the deck
  // DDGI's probe rays see the clear-sky cubemap, not the cloud deck, and fill
  // the open ground with blue-white noon bounce; the flat ambient path stages
  // the gloom correctly here (same reasoning as the water demo).
  rs.ddgi = false;
  // ...and the SSGI fallback splats the bright horizon band across the flat
  // lot the same way. The staged flat ambient carries the overcast fill.
  rs.ssgi = false;
  // SSR mirrors the bright horizon over the whole rough lot; the wet-ground
  // sheen in surface_weather is the reflection story for this scene.
  rs.ssr = false;
  // The aerial-perspective haze is tuned for kilometre vistas; over this 80 m
  // lot it veils the dark wet ground into milk. Keep a trace of it.
  rs.aerial_perspective = 0.15f;
  rs.auto_exposure = false;
  rs.exposure = 0.7f;

  // Rain means a thunderstorm: the demo plays the game's role and schedules
  // strikes (rx renders everything about them). The test hook fires the first
  // strike after exactly 1 s; the random storm rolls one every 6-9 s.
  weather_scene_ = true;
  storm_enabled_ = rs.weather.precipitation > 0.0f && !rs.weather.snow;
  storm_next_strike_ = WeatherStrikeTest.get() > 0.0f ? 1.0f : 4.0f;

  // Wide shot: shelter left, blocks right, plenty of open ground for splashes.
  camera_.set_position({6.0f, 2.8f, 9.6f});
  camera_.set_yaw_pitch(-0.62f, -0.16f);
  camera_.speed = 4.0f;
  RX_INFO("weather demo: precip {} ({}), wind {} m/s{}",
          rs.weather.precipitation, rs.weather.snow ? "snow" : "rain", rs.weather.wind_speed,
          storm_enabled_ ? ", thunderstorm on" : "");
}

// --demo sky: pin one weather state (-1 = free-running scheduler).
static base::Option<int> SkyState{"demo.sky.state", -1, "RX_SKY_STATE"};
// Ground-haze overrides on top of the pinned state (<0 = keep the state's
// values). RX_SKY_FOG=0.55 RX_SKY_FOG_H=18 turns a clear morning into
// knee-deep valley mist the towers rise out of.
static base::Option<float> SkyFog{"demo.sky.fog", -1.0f, "RX_SKY_FOG"};
static base::Option<float> SkyFogH{"demo.sky.fogh", -1.0f, "RX_SKY_FOG_H"};
// Camera start height override (>0): climb above the mist blanket and look
// down on the lot for the classic towers-piercing-the-fog framing.
static base::Option<float> SkyCamY{"demo.sky.camy", -1.0f, "RX_SKY_CAM_Y"};
// Storm-chaser cam: while a funnel is down, keep the camera aimed at it (the
// spawn ring is player-relative and random, so captures can't pre-aim).
static base::Option<bool> SkyTrack{"demo.sky.track", false, "RX_SKY_TRACK"};
// Short dwell/transitions so the scheduler's state changes fit a capture.
static base::Option<bool> SkyFast{"demo.sky.fast", false, "RX_SKY_FAST"};

void DemoScenes::CreateSkyDemoScene() {
  // Cloudscape acceptance scene: a wide flat lot with a few tall blocks for
  // scale/shadow reference; everything interesting happens in the sky. The
  // weather layer owns all atmospheric settings from here on (EmitSky).
  auto mat = [&](const char* tag, f32 r, f32 g, f32 b, f32 rough) {
    asset::Material m;
    m.id = asset::MakeAssetId(tag);
    m.base_color_factor[0] = r;
    m.base_color_factor[1] = g;
    m.base_color_factor[2] = b;
    m.roughness_factor = rough;
    m.metallic_factor = 0.0f;
    if (!config_.headless) renderer_.UploadMaterial(m);
    return m.id;
  };
  asset::AssetId ground_mat = mat("builtin/sky/ground", 0.30f, 0.34f, 0.26f, 0.95f);
  asset::AssetId block_mat = mat("builtin/sky/block", 0.42f, 0.40f, 0.38f, 0.8f);

  auto add_box = [&](asset::Mesh mesh, asset::AssetId material, Vec3 pos) {
    asset::MeshLod& lod = mesh.lods[0];  // MakeBox leaves the submesh list empty
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity e = world_.Create();
    world_.Add(e, scene::Transform{.position = {pos.x, pos.y, pos.z}});
    world_.Add(e, scene::Renderable{mesh.id});
  };
  add_box(asset::MakeBox(400.0f, 0.5f, 400.0f, asset::MakeAssetId("builtin/sky/ground_m")),
          ground_mat, {0, -0.5f, 0});
  add_box(asset::MakeBox(3.0f, 24.0f, 3.0f, asset::MakeAssetId("builtin/sky/tower_a")),
          block_mat, {26.0f, 12.0f, -18.0f});
  add_box(asset::MakeBox(2.0f, 12.0f, 2.0f, asset::MakeAssetId("builtin/sky/tower_b")),
          block_mat, {-30.0f, 6.0f, 8.0f});
  add_box(asset::MakeBox(6.0f, 3.0f, 6.0f, asset::MakeAssetId("builtin/sky/slab")),
          block_mat, {-6.0f, 1.5f, -34.0f});

  // The weather layer: four states spanning the model's range. Dwell and
  // transition times shrink under RX_SKY_FAST so a capture sees a change.
  weather_sys_ = std::make_unique<weather::WeatherSystem>(7u);
  const bool fast = bool(SkyFast);
  auto tune = [&](weather::WeatherState s) {
    if (fast) {
      s.transition_seconds = 6.0f;
      s.min_dwell = 8.0f;
      s.max_dwell = 16.0f;
    }
    return s;
  };
  // Shell altitudes follow the class each state represents: fair-weather
  // cumulus rides a high dry-climate condensation level, the stratocumulus
  // overcast is a genuinely low thin ceiling, and the storm is a
  // cumulonimbus tower running from a low base to a ~12 km anvil.
  weather::WeatherState clear;
  clear.name = "clear";
  clear.coverage = 0.26f;
  clear.cloud_type = 0.72f;
  clear.map_seed = 11u;
  clear.wind_speed = 9.0f;
  clear.base_altitude = 2200.0f;
  clear.top_altitude = 4600.0f;
  clear.fog_density = 0.04f;
  weather_sys_->AddState(tune(clear));
  weather::WeatherState scattered;
  scattered.name = "scattered";
  scattered.coverage = 0.48f;
  scattered.cloud_type = 0.78f;
  scattered.map_seed = 23u;
  scattered.wind_speed = 13.0f;
  scattered.base_altitude = 2000.0f;
  scattered.top_altitude = 6200.0f;
  scattered.fog_density = 0.1f;
  weather_sys_->AddState(tune(scattered));
  weather::WeatherState overcast;
  overcast.name = "overcast";
  overcast.coverage = 0.72f;
  overcast.cloud_type = 0.24f;
  overcast.density = 1.15f;
  overcast.map_seed = 37u;
  overcast.wind_speed = 16.0f;
  overcast.base_altitude = 1100.0f;
  overcast.top_altitude = 2800.0f;
  overcast.fog_density = 0.28f;  // damp grey day: the ceiling breathes down
  overcast.fog_height = 120.0f;
  weather_sys_->AddState(tune(overcast));
  weather::WeatherState storm;
  storm.name = "storm";
  storm.coverage = 0.8f;
  storm.cloud_type = 0.95f;
  storm.precipitation = 0.85f;
  storm.storminess = 0.9f;
  storm.darkness = 0.55f;  // menacing, not merely grey
  storm.density = 1.15f;
  storm.map_seed = 53u;
  storm.wind_speed = 22.0f;
  storm.turbulence = 1.4f;
  storm.base_altitude = 1500.0f;
  storm.top_altitude = 12000.0f;
  storm.fog_density = 0.34f;  // storm murk (post-rain mist rises on top)
  storm.fog_height = 140.0f;
  weather_sys_->AddState(tune(storm));
  // A distant front: the Unwetter sits kilometres off. No rain reaches the
  // player, the menace rides the far storm cells only (local deck keeps its
  // daylight), and strikes land in a far ring -- their flash glows on that
  // horizon and the thunder arrives many seconds late and muffled.
  weather::WeatherState front;
  front.name = "distant front";
  front.coverage = 0.52f;
  front.cloud_type = 0.85f;
  front.precipitation = 0.0f;
  front.storminess = 0.8f;
  front.darkness = 0.85f;
  front.map_seed = 71u;
  front.wind_speed = 17.0f;
  front.turbulence = 1.2f;
  front.base_altitude = 1700.0f;
  front.top_altitude = 10500.0f;
  front.fog_density = 0.12f;  // pre-storm stillness, light haze
  front.strike_min_range = 2500.0f;
  front.strike_max_range = 7000.0f;
  weather_sys_->AddState(tune(front));
  // Twister: a supercell-grade storm that spawns vortices. The funnel touches
  // down upwind, snakes past the lot and ropes out; strikes stay active.
  weather::WeatherState twister;
  twister.name = "twister";
  twister.coverage = 0.85f;
  twister.cloud_type = 0.95f;
  twister.precipitation = 0.55f;
  twister.storminess = 0.95f;
  twister.darkness = 0.5f;
  twister.density = 1.2f;
  twister.map_seed = 89u;
  twister.wind_speed = 18.0f;
  twister.turbulence = 1.5f;
  twister.base_altitude = 1400.0f;
  twister.top_altitude = 12000.0f;
  twister.fog_density = 0.18f;
  twister.tornado_prone = 1.0f;
  twister.weight = 0.4f;  // rare in the free-running schedule
  weather_sys_->AddState(tune(twister));

  int pinned = SkyState.get();
  if (pinned >= 0 && pinned < 6) {
    weather_sys_->ForceState(static_cast<u32>(pinned), 0.0f);
  }

  auto& rs = renderer_.settings();
  rs.cloudscape = true;
  rs.clouds = false;

  sky_scene_ = true;
  camera_.set_position({0.0f, 3.0f, 46.0f});
  camera_.set_yaw_pitch(0.0f, 0.12f);
  if (SkyCamY.get() > 0.0f) {
    camera_.set_position({0.0f, SkyCamY.get(), 110.0f});
    camera_.set_yaw_pitch(0.0f, -0.22f);
  }
  camera_.speed = 14.0f;
  RX_INFO("sky demo: cloudscape on, {} weather states{}", 6,
          pinned >= 0 ? " (pinned)" : "");
}

void DemoScenes::CreateSwampDemoScene() {
  // A stagnant lowland under a low stratus lid: dark waterlogged ground with
  // puddle sheen, leaning dead snags, and knee-deep mist drifting between
  // them. Everything atmospheric comes from one forced swamp weather state --
  // the scene itself is just wet geometry for the haze to sit in.
  auto mat = [&](const char* tag, f32 r, f32 g, f32 b, f32 rough) {
    asset::Material m;
    m.id = asset::MakeAssetId(tag);
    m.base_color_factor[0] = r;
    m.base_color_factor[1] = g;
    m.base_color_factor[2] = b;
    m.roughness_factor = rough;
    m.metallic_factor = 0.0f;
    if (!config_.headless) renderer_.UploadMaterial(m);
    return m.id;
  };
  asset::AssetId mud_mat = mat("builtin/swamp/mud", 0.10f, 0.115f, 0.08f, 0.55f);
  asset::AssetId snag_mat = mat("builtin/swamp/snag", 0.16f, 0.13f, 0.10f, 0.9f);
  asset::AssetId moss_mat = mat("builtin/swamp/moss", 0.13f, 0.17f, 0.09f, 0.85f);

  auto add_box = [&](const char* tag, f32 w, f32 h, f32 d, asset::AssetId material, Vec3 pos,
                     f32 tilt_x, f32 tilt_z) {
    asset::Mesh mesh = asset::MakeBox(w, h, d, asset::MakeAssetId(tag));
    asset::MeshLod& lod = mesh.lods[0];
    lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), material});
    if (!config_.headless) renderer_.UploadMesh(mesh);
    ecs::Entity e = world_.Create();
    scene::Transform t;
    t.position[0] = pos.x;
    t.position[1] = pos.y;
    t.position[2] = pos.z;
    // Small-angle lean as a quaternion straight from the half-angles: the
    // snags list drunkenly instead of standing surveyor-straight.
    f32 hx = tilt_x * 0.5f, hz = tilt_z * 0.5f;
    t.rotation[0] = std::sin(hx);
    t.rotation[2] = std::sin(hz);
    t.rotation[3] = std::cos(hx) * std::cos(hz);
    world_.Add(e, t);
    world_.Add(e, scene::Renderable{mesh.id});
  };

  add_box("builtin/swamp/ground", 300.0f, 0.5f, 300.0f, mud_mat, {0, -0.5f, 0}, 0, 0);
  // Hummocks: low mossy mounds breaking the plane.
  u32 rng = 0x51ab7u;
  auto next01 = [&rng]() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<f32>(rng >> 8) * (1.0f / 16777216.0f);
  };
  for (int i = 0; i < 10; ++i) {
    f32 x = (next01() - 0.5f) * 120.0f;
    f32 z = (next01() - 0.5f) * 120.0f;
    f32 w = 2.5f + next01() * 4.0f;
    char tag[64];
    std::snprintf(tag, sizeof(tag), "builtin/swamp/hummock_%d", i);
    add_box(tag, w, 0.5f + next01() * 0.5f, w * (0.7f + next01() * 0.6f), moss_mat,
            {x, 0.15f, z}, 0, 0);
  }
  // Dead snags: tall thin trunks with a drunken lean, some snapped short.
  for (int i = 0; i < 26; ++i) {
    f32 x = (next01() - 0.5f) * 140.0f;
    f32 z = (next01() - 0.5f) * 140.0f;
    if (std::fabs(x) < 4.0f && std::fabs(z) < 4.0f) continue;  // keep the spawn clear
    f32 h = next01() < 0.3f ? 3.0f + next01() * 3.0f : 8.0f + next01() * 9.0f;
    f32 w = 0.25f + next01() * 0.35f;
    f32 tx = (next01() - 0.5f) * 0.16f;
    f32 tz = (next01() - 0.5f) * 0.16f;
    char tag[64];
    std::snprintf(tag, sizeof(tag), "builtin/swamp/snag_%d", i);
    add_box(tag, w, h, w, snag_mat, {x, h * 0.5f, z}, tx, tz);
  }

  // One forced swamp state: low thin stratus lid, still air, thick shallow
  // mist. Wetness is pinned in EmitSky so the mud keeps its puddle sheen.
  weather_sys_ = std::make_unique<weather::WeatherSystem>(3u);
  weather::WeatherState swamp;
  swamp.name = "swamp";
  swamp.coverage = 0.68f;
  swamp.cloud_type = 0.2f;
  swamp.map_seed = 97u;
  swamp.wind_speed = 3.5f;   // stagnant air: the mist barely crawls
  swamp.base_altitude = 800.0f;
  swamp.top_altitude = 2000.0f;
  swamp.fog_density = SkyFog.get() >= 0.0f ? SkyFog.get() : 0.55f;
  swamp.fog_height = SkyFogH.get() > 0.0f ? SkyFogH.get() : 13.0f;
  swamp.fog_churn = 0.8f;  // vapour boiling off the standing water
  weather_sys_->AddState(swamp);
  weather_sys_->ForceState(0, 0.0f);

  auto& rs = renderer_.settings();
  rs.cloudscape = true;
  rs.clouds = false;

  sky_scene_ = true;
  swamp_scene_ = true;
  camera_.set_position({2.0f, 2.6f, 34.0f});
  camera_.set_yaw_pitch(-0.12f, 0.03f);
  camera_.speed = 8.0f;
  RX_INFO("swamp demo: forced swamp state, fog {:.2f} H {:.0f} m",
          weather_sys_->cloudscape().fog_density, weather_sys_->cloudscape().fog_height);
}

void DemoScenes::EmitSky(f32 dt) {
  if (!weather_sys_) return;
  sky_time_ += dt;
  // The viewer's world clock drives the sun; the layer only needs a rough day
  // phase for its day/night weight bias. A fixed early afternoon keeps the
  // demo deterministic.
  weather_sys_->Update(dt, camera_.position(), 0.45f);
  auto& rs = renderer_.settings();
  rs.cloudscape_controls = weather_sys_->cloudscape();
  rs.weather = weather_sys_->weather();
  if (SkyFog.get() >= 0.0f) rs.cloudscape_controls.fog_density = SkyFog.get();
  if (SkyFogH.get() > 0.0f) rs.cloudscape_controls.fog_height = SkyFogH.get();
  if (swamp_scene_) {
    // Standing water never dries: pin the surface response so the mud keeps
    // its puddle sheen without any rain falling.
    rs.weather.wetness = std::max(rs.weather.wetness, 0.8f);
  }
  // Touchdown breadcrumb: aim RX_CAM at the funnel for deterministic shots.
  const render::CloudscapeControls& cc = rs.cloudscape_controls;
  if (bool(SkyTrack) && cc.tornado_strength > 0.02f) {
    Vec3 cam = camera_.position();
    f32 dx = cc.tornado_pos.x - cam.x;
    f32 dz = cc.tornado_pos.y - cam.z;
    f32 flat = std::sqrt(dx * dx + dz * dz);
    // forward() is (sin yaw, sin pitch, -cos yaw): aim a third up the funnel.
    camera_.set_yaw_pitch(std::atan2(dx, -dz), std::atan2(300.0f - cam.y, flat));
  }
  if (cc.tornado_strength > 0.05f && !sky_tornado_seen_) {
    sky_tornado_seen_ = true;
    RX_INFO("tornado touchdown at ({:.0f}, {:.0f}), radius {:.0f} m", cc.tornado_pos.x,
            cc.tornado_pos.y, cc.tornado_radius);
  } else if (cc.tornado_strength <= 0.01f) {
    sky_tornado_seen_ = false;
  }

  // Thunder (the game's role, like the strike scheduling itself): each new
  // strike queues a procedural clap delayed by the speed of sound, so the
  // flash leads the sound the way it does outdoors -- a 340 m strike rumbles
  // in a second later.
  const render::WeatherSettings& w = rs.weather;
  bool new_strike = w.strike_age >= 0.0f &&
                    (sky_prev_strike_age_ < 0.0f || w.strike_age < sky_prev_strike_age_);
  sky_prev_strike_age_ = w.strike_age;
  if (new_strike) {
    Vec3 cam = camera_.position();
    f32 dx = w.strike_pos.x - cam.x, dz = w.strike_pos.z - cam.z;
    f32 dist = std::sqrt(dx * dx + dz * dz);
    sky_thunder_.push_back({dist / 343.0f, w.strike_pos, w.strike_seed, w.strike_energy, dist});
  }
  for (u32 i = 0; i < sky_thunder_.size();) {
    sky_thunder_[i].delay -= dt;
    if (sky_thunder_[i].delay > 0.0f) {
      ++i;
      continue;
    }
    if (ctx_.audio && ctx_.audio->active()) {
      const PendingThunder& th = sky_thunder_[i];
      audio::PlayParams params;
      params.gain = 0.9f + 0.5f * th.energy;
      params.positional = true;
      // The rumble comes from the channel and the deck above it, not from the
      // ground scorch point; raising the source keeps it overhead-ish.
      params.position = Vec3{th.pos.x, th.pos.y + 400.0f, th.pos.z};
      params.atten = {200.0f, 9000.0f};
      u32 rate = ctx_.audio->mixer().output_rate();
      ctx_.audio->mixer().Play(audio::MakeThunder(rate, th.seed, th.energy, th.dist), params);
    }
    sky_thunder_[i] = sky_thunder_.back();
    sky_thunder_.pop_back();
  }
}

void DemoScenes::UpdateStorm(f32 dt) {
  // Demo-side strike scheduler, mirroring what a game does: pick a strike
  // position/seed/energy, advance strike_age each frame, and drive the GLOBAL
  // weather.lightning scalar (sun/ambient/cloud boosts) with the same envelope
  // as the bolt + positioned flash so all three flicker in lockstep. The
  // global term adds a slower exp(-age*9) afterglow under the stroke flicker.
  render::WeatherSettings& w = renderer_.settings().weather;
  storm_time_ += dt;

  if (w.strike_age >= 0.0f) {
    w.strike_age += dt;
    f32 env = render::LightningSystem::Envelope(w.strike_age, w.strike_seed);
    // Peak ~0.35: the global consumers multiply hard (sun +9x, ambient +0.5
    // at flash 1.0) and would white the staged overcast frame out above that.
    w.lightning = std::min(
        1.0f, w.strike_energy * (0.22f * std::exp(-w.strike_age * 9.0f) + 0.35f * env));
    if (w.strike_age > 1.2f) {  // envelope + afterglow both spent
      w.strike_age = -1.0f;
      w.lightning = 0.0f;
    }
  }

  if (storm_time_ < storm_next_strike_) return;
  auto hash01 = [](u32 v) {
    v = v * 747796405u + 2891336453u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return static_cast<f32>((v >> 22u) ^ v) * (1.0f / 4294967295.0f);
  };
  ++w.strike_seed;
  w.strike_age = 0.0f;
  const f32 test_dist = WeatherStrikeTest.get();
  if (test_dist > 0.0f) {
    // Deterministic: directly in front of the camera at the given distance.
    Vec3 fwd = camera_.forward();
    f32 len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    Vec3 dir = len > 1e-4f ? Vec3{fwd.x / len, 0.0f, fwd.z / len} : Vec3{0, 0, -1};
    Vec3 eye = camera_.position();
    w.strike_pos = {eye.x + dir.x * test_dist, 0.0f, eye.z + dir.z * test_dist};
    w.strike_energy = 1.0f;
    storm_next_strike_ = storm_time_ + 4.0f;
  } else {
    // Random storm: 300-900 m out at a hashed azimuth, every 6-9 s.
    f32 az = hash01(w.strike_seed * 3u + 1u) * 6.2831853f;
    f32 dist = 300.0f + 600.0f * hash01(w.strike_seed * 3u + 2u);
    Vec3 eye = camera_.position();
    w.strike_pos = {eye.x + std::cos(az) * dist, 0.0f, eye.z + std::sin(az) * dist};
    w.strike_energy = 0.65f + 0.35f * hash01(w.strike_seed * 3u + 3u);
    storm_next_strike_ = storm_time_ + 6.0f + 3.0f * hash01(w.strike_seed * 7u + 5u);
  }
}

void DemoScenes::CreateMeshletDemoScene() {
  // A dense sphere rendered through the mesh-shader meshlet path: the gpu splits
  // it into clusters, frustum/cone-culls them, and tints each a distinct color
  // so the decomposition is visible. The mesh is not a normal Renderable; the
  // renderer draws it via the meshlet pass (watch "meshlet: N meshlets ...").
  asset::Mesh sphere = asset::MakeSphere(1.5f, 64, 128, asset::MakeAssetId("builtin/meshlet/sphere"));
  if (!config_.headless) renderer_.UploadMeshletMesh(sphere);

  camera_.set_position({0.0f, 0.0f, 4.5f});
  camera_.set_yaw_pitch(0.0f, 0.0f);
  camera_.speed = 3.0f;
  RX_INFO("meshlet demo: mesh-shader cluster rendering ({} tris)",
           sphere.lods[0].indices.size() / 3);
}

void DemoScenes::CreateMaterialXDemoScene() {
  // One sphere per MaterialX file listed (comma separated) in RX_MTLX, so the
  // imported standard_surface lobes can be eyeballed against the source.
  asset::Mesh ground = asset::MakeCube(8.0f, asset::MakeAssetId("builtin/mtlx/ground"));
  for (asset::MeshLod& lod : ground.lods) {
    if (lod.submeshes.empty()) lod.submeshes.push_back({0, static_cast<u32>(lod.indices.size()), {}});
  }
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -8.6f, 0}});  // top at y = -0.6
  world_.Add(floor, scene::Renderable{ground.id});

  base::Vector<std::string> paths;
  if (const char* env = Mtlx.get()) {
    std::string s = env, cur;
    for (char c : s) {
      if (c == ',') {
        if (!cur.empty()) paths.push_back(cur);
        cur.clear();
      } else {
        cur.push_back(c);
      }
    }
    if (!cur.empty()) paths.push_back(cur);
  }
  if (paths.empty()) RX_WARN("mtlx demo: set RX_MTLX=a.mtlx,b.mtlx to load materials");

  int n = static_cast<int>(paths.size());
  for (int i = 0; i < n; ++i) {
    asset::Material mat;
    mat.id = asset::MakeAssetId("builtin/mtlx/mat" + std::to_string(i));
    if (!asset::LoadMaterialX(paths[i], &mat)) continue;
    if (!config_.headless) renderer_.UploadMaterial(mat);
    std::string tag = "builtin/mtlx/sphere" + std::to_string(i);
    asset::Mesh sphere = asset::MakeSphere(0.6f, 40, 60, asset::MakeAssetId(tag));
    sphere.lods[0].submeshes[0].material = mat.id;
    if (!config_.headless) renderer_.UploadMesh(sphere);
    ecs::Entity e = world_.Create();
    f32 x = (static_cast<f32>(i) - (n - 1) * 0.5f) * 1.5f;
    world_.Add(e, scene::Transform{.position = {x, 0.0f, 0.0f}});
    world_.Add(e, scene::Renderable{sphere.id});
  }

  camera_.set_position({0.0f, 0.9f, 4.5f});
  camera_.set_yaw_pitch(0.0f, -0.12f);
  camera_.speed = 3.0f;
  RX_INFO("materialx demo: {} materials", n);
}

void DemoScenes::CreateDemoScene() {
  if (config_.demo_scene == "water") {
    CreateWaterDemoScene();
    return;
  }
  if (config_.demo_scene == "fluid") {
    CreateFluidDemoScene();
    return;
  }
  if (config_.demo_scene == "sky") {
    CreateSkyDemoScene();
    return;
  }
  if (config_.demo_scene == "swamp") {
    CreateSwampDemoScene();
    return;
  }
  if (config_.demo_scene == "weather") {
    CreateWeatherDemoScene();
    return;
  }
  if (config_.demo_scene == "cornell") {
    CreateCornellDemoScene();
    return;
  }
  if (config_.demo_scene == "interior") {
    CreateInteriorDemoScene();
    return;
  }
  if (config_.demo_scene == "fur") {
    CreateFurDemoScene();
    return;
  }
  if (config_.demo_scene == "gpuparticles") {
    CreateGpuParticleDemoScene();
    return;
  }
  if (config_.demo_scene == "fire") {
    CreateFireDemoScene();
    return;
  }
  if (config_.demo_scene == "bricks") {
    CreateBrickDemoScene();
    return;
  }
  if (config_.demo_scene == "silpom") {
    CreateSilhouettePomDemoScene();
    return;
  }
  if (config_.demo_scene == "vt") {
    CreateVirtualTextureDemoScene();
    return;
  }
  if (config_.demo_scene == "vgeo") {
    CreateVirtualGeometryDemoScene();
    return;
  }
  if (config_.demo_scene == "strands") {
    CreateStrandHairDemoScene();
    return;
  }
  if (config_.demo_scene == "cloth") {
    CreateClothDemoScene();
    return;
  }
  if (config_.demo_scene == "imposters") {
    CreateImposterDemoScene();
    return;
  }
  if (config_.demo_scene == "sss") {
    CreateSssDemoScene();
    return;
  }
  if (config_.demo_scene == "autolod") {
    CreateAutoLodDemoScene();
    return;
  }
  if (config_.demo_scene == "mtlx") {
    CreateMaterialXDemoScene();
    return;
  }
  if (config_.demo_scene == "oit") {
    CreateOitDemoScene();
    return;
  }
  if (config_.demo_scene == "materials") {
    CreateMaterialDemoScene();
    return;
  }
  if (config_.demo_scene == "gaussian") {
    CreateGaussianDemoScene();
    return;
  }
  if (config_.demo_scene == "lod") {
    CreateLodDemoScene();
    return;
  }
  if (config_.demo_scene == "occlusion") {
    CreateOcclusionDemoScene();
    return;
  }
  if (config_.demo_scene == "meshlet") {
    CreateMeshletDemoScene();
    return;
  }
  if (config_.demo_scene == "lights") {
    CreatePointLightDemoScene();
    return;
  }
  if (config_.demo_scene == "locomotion") {
    CreateLocomotionDemoScene();
    return;
  }
  if (config_.demo_scene == "scenehook") {
    CreateSceneHookDemoScene();
    return;
  }
  if (config_.demo_scene == "scenehook-rhi") {
    CreateSceneHookRhiDemoScene();
    return;
  }
  if (config_.demo_scene == "bubbles") {
    CreateBubbleDemoScene();
    return;
  }
  if (config_.demo_scene == "ship") {
    ship_ = std::make_unique<ShipDemo>(ctx_);
    ship_->Create();
    return;
  }
  if (config_.demo_scene == "nav") {
    nav_ = std::make_unique<NavDemo>(ctx_);
    nav_->Create();
    return;
  }
  if (config_.demo_scene == "placement") {
    placement_ = std::make_unique<PlacementDemo>(ctx_);
    placement_->Create();
    return;
  }
  if (config_.demo_scene == "gym") {
    gym_ = std::make_unique<GymDemo>(ctx_);
    gym_->Create();
    return;
  }
  if (config_.demo_scene == "puppet") {
    puppet_ = std::make_unique<PuppetDemo>(ctx_);
    puppet_->Create();
    return;
  }
  if (config_.demo_scene == "drive") {
    drive_ = std::make_unique<DriveDemo>(ctx_);
    drive_->Create();
    return;
  }
  asset::Mesh cube = asset::MakeCube(0.7f, asset::MakeAssetId("builtin/cube"));
  asset::Mesh ground = asset::MakeCube(2.5f, asset::MakeAssetId("builtin/ground"));
  if (!config_.headless) {
    renderer_.UploadMesh(cube);
    renderer_.UploadMesh(ground);
  }

  ecs::Entity entity = world_.Create();
  world_.Add(entity, scene::Transform{.position = {-2.4f, 0.5f, 0}});
  world_.Add(entity, scene::Renderable{cube.id});
  world_.Add(entity, Spin{});

  // Ground under the cube so raytraced shadows have something to land on.
  ecs::Entity floor = world_.Create();
  world_.Add(floor, scene::Transform{.position = {0, -3.6f, 0}});
  world_.Add(floor, scene::Renderable{ground.id});

  scheduler_.AddSystem(ecs::Stage::kSim, "demo_spin", [](ecs::World& world, f32 dt) {
    world.Each<Spin, scene::Transform>([dt](ecs::Entity, Spin& spin, scene::Transform& t) {
      spin.angle += spin.speed * dt;
      t.rotation[1] = std::sin(spin.angle * 0.5f);
      t.rotation[3] = std::cos(spin.angle * 0.5f);
    });
  });
  RX_INFO("no scene given, spinning a cube instead");
}

namespace {
// FNV-1a, matching kinema::HashName, so the demo can name a curve without
// pulling in kinema (which stays a private detail of engine/anim).
constexpr u64 NameHash(const char* s) {
  u64 h = 14695981039346656037ull;
  while (*s) {
    h ^= static_cast<u8>(*s++);
    h *= 1099511628211ull;
  }
  return h;
}
}  // namespace

void DemoScenes::CreateLocomotionDemoScene() {
  locomotion_enabled_ = true;

  // The character: a blocky skinned biped and its rig, driven by a kinema graph.
  asset::Mesh biped;
  asset::MakeSkinnedBiped(asset::MakeAssetId("builtin/locomotion/biped"), &loco_skeleton_, &biped);
  asset::Material skin_mat;
  skin_mat.id = asset::MakeAssetId("builtin/locomotion/biped_mat");
  skin_mat.base_color_factor[0] = 0.64f;
  skin_mat.base_color_factor[1] = 0.46f;
  skin_mat.base_color_factor[2] = 0.36f;
  skin_mat.roughness_factor = 0.7f;
  biped.lods[0].submeshes[0].material = skin_mat.id;
  loco_skin_ = biped.skin;
  loco_mesh_ = biped.id.hash;
  if (!config_.headless) {
    renderer_.UploadMaterial(skin_mat);
    renderer_.UploadMesh(biped);
  }

  // Shared animation archetype; per-character player + foot placement bound to it.
  loco_graph_ = anim::BuildBipedLocomotionGraph(loco_skeleton_);
  loco_rig_.Bind(loco_graph_);
  loco_feet_.Bind(loco_graph_, 0.08f);
  loco_remap_ = anim::BuildBoneRemap(loco_skeleton_, loco_skin_);
  loco_pose_.ResetToBind(loco_skeleton_);
  loco_pos_ = {0, 0, -2.0f};
  loco_yaw_ = 0.0f;  // facing +Z, the walk direction

  // Ground + collision (foot IK raycasts against it).
  asset::Material gmat;
  gmat.id = asset::MakeAssetId("builtin/locomotion/ground_mat");
  gmat.base_color_factor[0] = 0.32f;
  gmat.base_color_factor[1] = 0.34f;
  gmat.base_color_factor[2] = 0.30f;
  gmat.roughness_factor = 0.95f;
  if (!config_.headless) renderer_.UploadMaterial(gmat);

  asset::Mesh ground = asset::MakeBox(20.0f, 0.5f, 20.0f, asset::MakeAssetId("builtin/loco/ground"));
  ground.lods[0].submeshes.push_back({0, static_cast<u32>(ground.lods[0].indices.size()), gmat.id});
  if (!config_.headless) renderer_.UploadMesh(ground);
  ecs::Entity g = world_.Create();
  world_.Add(g, scene::Transform{.position = {0, -0.5f, 0}});  // top surface at y = 0
  world_.Add(g, scene::Renderable{ground.id});
  physics_.AddStaticBox({0, -0.5f, 0}, {20.0f, 0.5f, 20.0f});

  // A run of uneven bumps along the +Z walk path for the foot placement to adapt
  // to (each box's top is its height above the ground plane).
  asset::Mesh bump = asset::MakeBox(0.55f, 0.12f, 0.7f, asset::MakeAssetId("builtin/loco/bump"));
  bump.lods[0].submeshes.push_back({0, static_cast<u32>(bump.lods[0].indices.size()), gmat.id});
  if (!config_.headless) renderer_.UploadMesh(bump);
  const f32 heights[5] = {0.07f, 0.15f, 0.10f, 0.19f, 0.09f};
  for (int i = 0; i < 5; ++i) {
    f32 z = 0.6f + static_cast<f32>(i) * 1.45f;
    f32 x = (i % 2 == 0) ? -0.14f : 0.14f;
    f32 cy = heights[i] - 0.12f;  // box centre so its top sits at `heights[i]`
    ecs::Entity be = world_.Create();
    world_.Add(be, scene::Transform{.position = {x, cy, z}});
    world_.Add(be, scene::Renderable{bump.id});
    physics_.AddStaticBox({x, cy, z}, {0.55f, 0.12f, 0.7f});
  }

  // Side-on camera framing the path.
  camera_.set_position({-4.5f, 2.8f, 3.0f});
  camera_.set_yaw_pitch(1.5708f, -0.35f);
  RX_INFO("locomotion demo: kinema-driven biped (idle/walk/run blend + foot IK)");
}

void DemoScenes::EmitLocomotion(f32 dt, render::FrameView& view) {
  (void)dt;
  // Fixed step so a headless capture animates deterministically by frame count
  // regardless of wall-clock frame time.
  const f32 sdt = 1.0f / 60.0f;
  loco_time_ += sdt;

  // Speed: the viewer's move axes when the player drives, else a scripted
  // idle->walk->run ramp so an unattended capture still shows locomotion.
  f32 speed = 0.0f;
  bool input_driven = false;
  if (ctx_.actions) {
    f32 mx = ctx_.actions->axis(Axis::kMoveX);
    f32 my = ctx_.actions->axis(Axis::kMoveY);
    f32 mag = std::sqrt(mx * mx + my * my);
    if (mag > 0.05f) {
      speed = std::min(mag, 1.0f) * 4.5f;  // full stick -> run
      input_driven = true;
    }
  }
  if (!input_driven) {
    f32 t = loco_time_;
    if (t < 0.7f) speed = 0.0f;
    else if (t < 1.4f) speed = (t - 0.7f) / 0.7f * 1.6f;       // ramp into a walk
    else if (t < 2.0f) speed = 1.6f + (t - 1.4f) / 0.6f * 2.4f;  // ramp into a run
    else speed = 4.0f;
  }
  loco_rig_.SetSpeed(speed);

  // Advance the graph. Footfall markers + the footstep-intensity curve are
  // consumed here as a footstep log (the notify/curve path, demonstrably fired).
  Vec3 root = loco_rig_.Update(sdt, &loco_pose_, [&](const anim::RigPlayer::Event& e) {
    if (e.phase != anim::RigPlayer::Event::Phase::kPoint) return;
    ++loco_footsteps_;
    f32 intensity = loco_rig_.SampleCurve(NameHash("FootstepIntensity"), 0.5f);
    RX_INFO("footstep {} (#{}, intensity {:.2f})", e.name, loco_footsteps_, intensity);
  });

  // Root motion moves the entity (never teleported inside the anim layer). Loop
  // it back so it keeps re-crossing the bump run and stays framed.
  Quat facing = QuatFromAxisAngle({0, 1, 0}, loco_yaw_);
  loco_pos_ += Rotate(facing, root);
  if (loco_pos_.z > 8.0f) loco_pos_.z = -2.0f;

  // Foot placement: raycast the physics world under each foot. The probe works
  // in the actor's model space; convert to/from world with the actor transform
  // (physics stays out of engine/anim).
  Mat4 actor = MakeTransform(loco_pos_, facing, 1.0f);
  Mat4 inv = Inverse(actor);
  auto probe = [&](const Vec3& model_origin, Vec3* hit, Vec3* normal) -> bool {
    Vec3 world_origin = TransformPoint(actor, model_origin);
    physics::PhysicsWorld::RayHit rh;
    if (!physics_.Raycast(world_origin, {0, -1, 0}, 2.0f, &rh)) return false;
    *hit = TransformPoint(inv, rh.position);
    *normal = Normalize(TransformDir(inv, rh.normal));
    return true;
  };
  loco_feet_.Apply(&loco_pose_, probe);

  // Skin the SoA pose into a GPU palette and submit one skinned draw.
  anim::ComputeModelMatrices(loco_skeleton_, loco_pose_, &loco_bone_model_);
  anim::BuildSkinPalette(loco_bone_model_, loco_skin_, loco_remap_, &loco_palette_);
  i32 skin_offset = static_cast<i32>(view.bone_matrices.size());
  for (const Mat4& m : loco_palette_) view.bone_matrices.push_back(m);
  view.draws.push_back({loco_mesh_, actor, loco_prev_xform_, skin_offset});
  loco_prev_xform_ = actor;
}

}  // namespace rx
