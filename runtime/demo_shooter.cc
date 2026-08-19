#include "demo_shooter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "asset/material.h"
#include "asset/primitives.h"
#include "combat/projectile.h"
#include "character/character.h"
#include "core/log.h"
#include "ecs/world.h"
#include "scene/camera.h"
#include "scene/camera_rig.h"
#include "scene/components.h"

#if defined(RX_HAS_IMGUI)
#include <imgui.h>
#endif

// The FPS range: graybox gallery, four weapons cut from WeaponDef data, popup
// and strafing targets with head/torso hitboxes, physics crates, and a HUD that
// reads the same numbers the engine fires with.
namespace rx {
namespace {

f32 ShooterFixedStep() {
  if (const char* env = std::getenv("RX_FIXED_DT")) {
    const f32 v = std::strtof(env, nullptr);
    if (v > 0.0f) return v;
  }
  return 1.0f / 60.0f;
}

Mat4 ScaleMat(const Vec3& s) {
  Mat4 m = Mat4::Identity();
  m.m[0] = s.x;
  m.m[5] = s.y;
  m.m[10] = s.z;
  return m;
}

Mat4 BoxTransform(const Vec3& center, const Vec3& half_extent) {
  return MakeTranslation(center) * ScaleMat(half_extent * 2.0f);
}

// Character heading: yaw about -Y so the body agrees with the camera rig.
Quat HeadingQuat(f32 yaw) { return QuatFromAxisAngle({0, -1, 0}, yaw); }

u32 Rgba(u32 rgb, f32 alpha) {
  const u32 a = static_cast<u32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
  return (rgb << 8) | a;
}

// Erases every entry whose life has run out, in place.
template <typename T>
void AgeOut(base::Vector<T>& items, f32 dt) {
  for (mem_size i = items.size(); i > 0; --i) {
    T& item = items[i - 1];
    item.life -= dt;
    if (item.life <= 0) items.erase(i - 1);
  }
}

}  // namespace

ShooterDemo::ShooterDemo(EngineContext& ctx) : ctx_(ctx) {}

void ShooterDemo::Create() {
  sim_accum_ = 0.0f;
  if (!ctx_.config->headless) {
    auto& s = ctx_.renderer->settings();
    s.sun_direction = Normalize(Vec3{-0.35f, -0.9f, -0.3f});
    s.sun_intensity = 2.7f;
    s.sun_color = {1.0f, 0.97f, 0.92f};
    s.ambient = 0.22f;
    s.night = 0.0f;
    s.lens_flare = 0.0f;
    s.ssr = false;
  }
  ctx_.scene_owns_sun = true;

  BuildRange();
  BuildWeapons();
  BuildPlayer();

  if (const char* autofire = std::getenv("RX_SHOOTER_AUTOFIRE")) {
    autofire_ = std::strtol(autofire, nullptr, 10) != 0;
    if (autofire_) {
      mouse_captured_ = false;  // scripted run: leave the cursor alone
      RX_INFO("shooter: RX_SHOOTER_AUTOFIRE, holding the trigger on the nearest target");
    }
  }

  RX_INFO(
      "shooter demo: WASD move, mouse look, LMB fire, RMB aim, R reload, 1-4 / wheel weapons, "
      "Shift sprint, Ctrl crouch, Space jump, G reset, Tab release cursor");
}

void ShooterDemo::BuildRange() {
  physics::PhysicsWorld& phys = *ctx_.physics;

  asset::Mesh cube = asset::MakeCube(0.5f, asset::MakeAssetId("shooter/cube"));  // 1 m unit cube
  asset::Mesh ball = asset::MakeSphere(0.5f, 12, 20, asset::MakeAssetId("shooter/ball"));
  cube_mesh_ = cube.id.hash;
  sphere_mesh_ = ball.id.hash;
  if (!ctx_.config->headless) {
    ctx_.renderer->UploadMesh(cube);
    ctx_.renderer->UploadMesh(ball);
  }

  // The props list is drawn every frame as instanced unit cubes; the physics
  // boxes beside them are what the rounds and the character actually collide
  // with.
  auto solid = [&](const Vec3& center, const Vec3& half, u32 tint) {
    props_.push_back(Prop{BoxTransform(center, half), tint});
    phys.AddStaticBox(center, half);
  };

  solid({0, -0.5f, 0}, {30, 0.5f, 30}, 0x1e2126);       // floor
  solid({0, 3.0f, -30.5f}, {30, 3.5f, 0.5f}, 0x343941);  // butt wall behind the targets
  solid({-30.5f, 3.0f, 0}, {0.5f, 3.5f, 30}, 0x2b2f35);
  solid({30.5f, 3.0f, 0}, {0.5f, 3.5f, 30}, 0x2b2f35);
  solid({0, 3.0f, 16.5f}, {30, 3.5f, 0.5f}, 0x2b2f35);

  // Firing line and lane markers every 5 m, so distances are readable and the
  // damage falloff can be seen changing as you back off.
  for (int lane = 1; lane <= 5; ++lane) {
    const f32 z = 10.0f - lane * 5.0f;
    props_.push_back(Prop{BoxTransform({0, 0.01f, z}, {14.0f, 0.01f, 0.05f}), 0x8a9099});
  }

  // Cover: a thin plank the rifle punches through, and a thick block it cannot.
  solid({-4.0f, 1.0f, -6.0f}, {1.6f, 1.0f, 0.05f}, 0xb08040);  // 0.1 m: penetrable
  solid({4.0f, 1.0f, -6.0f}, {1.6f, 1.0f, 0.5f}, 0x6b4a2a);    // 1.0 m: cover
  // A low wall to crouch behind, and a shoulder-height one to lean the shots over.
  solid({0, 0.5f, 2.0f}, {5.0f, 0.5f, 0.3f}, 0x3f444c);
  solid({-9.0f, 1.4f, -2.0f}, {2.0f, 1.4f, 0.3f}, 0x3f444c);

  // Static targets down the lanes, then two that strafe and have to be led.
  SpawnTarget({-6.0f, 0, -10.0f}, 0);
  SpawnTarget({0, 0, -14.0f}, 0);
  SpawnTarget({6.0f, 0, -10.0f}, 0);
  SpawnTarget({-3.0f, 0, -22.0f}, 0);
  SpawnTarget({3.0f, 0, -22.0f}, 0);
  SpawnTarget({0, 0, -18.0f}, 5.0f);
  SpawnTarget({8.0f, 0, -26.0f}, 4.0f);

  // Crates: dynamic bodies registered with the hit registry so blasts pick them
  // up. They carry no entity, so they take impulses and never take damage.
  for (int i = 0; i < 8; ++i) {
    const Vec3 half{0.25f, 0.25f, 0.25f};
    const Vec3 position{-2.0f + static_cast<f32>(i % 4) * 1.2f, 0.3f + (i / 4) * 0.6f, -3.0f};
    const physics::BodyId body = phys.AddDynamicBox(position, half, 260.0f, {0, 0, 0});
    registry_.Register(body, ecs::kInvalidEntity);
    crates_.push_back(Crate{body, half, 0xa8804a});
  }
}

void ShooterDemo::BuildWeapons() {
  // Four weapons, all of them the same struct with different numbers. The
  // engine has no idea what a "shotgun" is.
  combat::WeaponDef rifle;
  rifle.name_hash = asset::MakeAssetId("shooter/rifle").hash;
  rifle.mode = combat::FireMode::kAuto;
  rifle.rpm = 660.0f;
  rifle.damage = 24.0f;
  rifle.range = 120.0f;
  rifle.falloff_start = 25.0f;
  rifle.falloff_end = 70.0f;
  rifle.falloff_min_scale = 0.55f;
  rifle.max_penetrations = 1;
  rifle.penetration = 0.2f;
  rifle.penetration_damage_scale = 0.6f;
  rifle.impulse = 12.0f;
  rifle.magazine = 30;
  rifle.reserve_max = 240;
  rifle.reload_time = 2.0f;
  rifle.reload_empty_time = 2.6f;
  rifle.spread_max = 0.035f;
  rifle.spread_per_shot = 0.07f;
  rifle.spread_decay = 2.2f;
  rifle_ = catalog_.Register(rifle);

  combat::WeaponDef shotgun;
  shotgun.name_hash = asset::MakeAssetId("shooter/shotgun").hash;
  shotgun.mode = combat::FireMode::kSemi;
  shotgun.rpm = 90.0f;
  shotgun.pellets = 9;
  shotgun.damage = 12.0f;
  shotgun.range = 40.0f;
  shotgun.falloff_start = 6.0f;
  shotgun.falloff_end = 22.0f;
  shotgun.falloff_min_scale = 0.2f;
  shotgun.impulse = 26.0f;
  shotgun.spread_min = 0.055f;
  shotgun.spread_max = 0.11f;
  shotgun.spread_per_shot = 0.5f;
  shotgun.spread_ads_scale = 0.6f;
  shotgun.recoil_pitch = 0.05f;
  shotgun.recoil_yaw_variance = 0.012f;
  shotgun.magazine = 6;
  shotgun.reserve_max = 60;
  shotgun.reload_shell_time = 0.42f;  // shell by shell, interruptible
  shotgun.ads_time = 0.28f;
  shotgun.ads_fov_scale = 0.9f;
  shotgun.swap_time = 0.6f;
  shotgun_ = catalog_.Register(shotgun);

  combat::WeaponDef marksman;
  marksman.name_hash = asset::MakeAssetId("shooter/marksman").hash;
  marksman.mode = combat::FireMode::kSemi;
  marksman.rpm = 220.0f;
  marksman.damage = 55.0f;
  marksman.range = 300.0f;
  marksman.falloff_start = 80.0f;
  marksman.falloff_end = 200.0f;
  marksman.falloff_min_scale = 0.8f;
  marksman.max_penetrations = 2;
  marksman.penetration = 0.35f;
  marksman.penetration_damage_scale = 0.75f;
  marksman.impulse = 30.0f;
  marksman.spread_min = 0.0016f;
  marksman.spread_max = 0.06f;
  marksman.spread_per_shot = 0.4f;
  marksman.spread_decay = 1.1f;
  marksman.spread_ads_scale = 0.12f;
  marksman.recoil_pitch = 0.035f;
  marksman.recoil_yaw_variance = 0.006f;
  marksman.recoil_ads_scale = 0.5f;
  marksman.magazine = 10;
  marksman.reserve_max = 80;
  marksman.reload_time = 2.4f;
  marksman.reload_empty_time = 3.0f;
  marksman.ads_time = 0.3f;
  marksman.ads_fov_scale = 0.45f;  // a real scope, not a nudge
  marksman.swap_time = 0.7f;
  marksman_ = catalog_.Register(marksman);

  combat::WeaponDef launcher;
  launcher.name_hash = asset::MakeAssetId("shooter/launcher").hash;
  launcher.kind = combat::WeaponKind::kProjectile;
  launcher.mode = combat::FireMode::kSemi;
  launcher.rpm = 60.0f;
  launcher.damage = 40.0f;   // direct hit, on top of the blast
  launcher.impulse = 60.0f;
  launcher.spread_min = 0.004f;
  launcher.spread_max = 0.03f;
  launcher.spread_per_shot = 0.6f;
  launcher.recoil_pitch = 0.06f;
  launcher.magazine = 4;
  launcher.reserve_max = 24;
  launcher.reload_time = 3.0f;
  launcher.muzzle_speed = 32.0f;
  launcher.projectile_gravity = 9.81f;
  launcher.projectile_drag = 0.002f;
  launcher.projectile_radius = 0.12f;
  launcher.projectile_life = 6.0f;
  launcher.explode_on_expire = true;  // a dud still goes off at the end of its fuse
  launcher.blast_radius = 5.0f;
  launcher.blast_damage = 110.0f;
  launcher.blast_min_scale = 0.15f;
  launcher.blast_impulse = 260.0f;
  launcher.ads_time = 0.25f;
  launcher.ads_fov_scale = 0.85f;
  launcher.swap_time = 0.8f;
  launcher_ = catalog_.Register(launcher);
}

void ShooterDemo::BuildPlayer() {
  ecs::World& world = *ctx_.world;
  physics::PhysicsWorld& phys = *ctx_.physics;

  character::CharacterShape shape;
  character::CharacterMovementSettings move;
  const f32 radius = shape.standing_radius;
  const f32 half_height = std::max(shape.standing_height * 0.5f - radius, 0.01f);

  player_ = world.Create();
  world.Add(player_, scene::Transform{.position = {spawn_feet_.x, spawn_feet_.y, spawn_feet_.z}});
  world.Add(player_, shape);
  world.Add(player_, move);
  world.Add(player_, character::CharacterIntent{});
  world.Add(player_, character::CharacterState{});
  world.Add(player_, character::CharacterViewMode{});  // first person

  const Vec3 center = spawn_feet_ + Vec3{0, shape.standing_height * 0.5f, 0};
  world.Add(player_, character::CharacterBody{phys.CreateCharacter(center, radius, half_height),
                                              radius, half_height, false});

  // The combat half: a loadout, the intent the game fills, the view kick, the
  // viewmodel pose, and enough health to survive standing in its own blast.
  combat::Loadout loadout;
  combat::GiveWeapon(loadout, catalog_, rifle_, 180);
  combat::GiveWeapon(loadout, catalog_, shotgun_, 42);
  combat::GiveWeapon(loadout, catalog_, marksman_, 60);
  combat::GiveWeapon(loadout, catalog_, launcher_, 16);
  world.Add(player_, loadout);
  world.Add(player_, combat::WeaponIntent{});
  world.Add(player_, combat::ViewRecoil{});
  world.Add(player_, combat::Viewmodel{});
  world.Add(player_, combat::Team{1, false});
  combat::Health health;
  health.regen_delay = 3.0f;
  health.regen_rate = 12.0f;
  world.Add(player_, health);
  world.Add(player_, combat::Damageable{});

  character::ApplyCharacterViewMode(world, player_);
  camera_output_ = world.Create();
  scene::InitializeCameraStack(world, camera_output_, player_);
  // Yaw 0 already faces -Z, which is down the range at the targets.
}

void ShooterDemo::SpawnTarget(const Vec3& feet, f32 strafe_span) {
  ecs::World& world = *ctx_.world;
  physics::PhysicsWorld& phys = *ctx_.physics;

  Target target;
  target.home = feet;
  target.strafe_span = strafe_span;
  target.strafe_phase = static_cast<f32>(targets_.size()) * 0.9f;

  target.entity = world.Create();
  world.Add(target.entity, scene::Transform{.position = {feet.x, feet.y, feet.z}});
  combat::Health health;
  health.hp = 100.0f;
  health.max_hp = 100.0f;
  world.Add(target.entity, health);
  combat::Damageable damageable;
  damageable.center_offset = {0, 0.95f, 0};
  world.Add(target.entity, damageable);
  world.Add(target.entity, combat::Team{2, false});

  // Two kinematic boxes stand in for a skeleton's hitboxes. Registering them is
  // the whole contract: a round that lands on the head body scores a head shot.
  target.torso = phys.AddKinematicBox(feet + Vec3{0, 0.95f, 0}, {0.3f, 0.55f, 0.18f});
  target.head = phys.AddKinematicBox(feet + Vec3{0, 1.72f, 0}, {0.13f, 0.13f, 0.13f});
  registry_.Register(target.torso, target.entity, combat::HitZone::kTorso);
  registry_.Register(target.head, target.entity, combat::HitZone::kHead);

  targets_.push_back(target);
}

void ShooterDemo::ResetPlayer() {
  character::TeleportCharacter(*ctx_.world, *ctx_.physics, player_, spawn_feet_);
  if (auto* health = ctx_.world->Get<combat::Health>(player_)) {
    health->hp = health->max_hp;
    health->dead = false;
  }
  if (auto* loadout = ctx_.world->Get<combat::Loadout>(player_)) {
    for (u8 i = 0; i < loadout->count; ++i) {
      const combat::WeaponDef* def = catalog_.Find(loadout->slots[i].def);
      if (!def) continue;
      loadout->slots[i].ammo = def->magazine;
      loadout->slots[i].reserve = def->reserve_max;
      loadout->slots[i].reload_timer = 0;
      loadout->slots[i].reload_shells = false;
    }
  }
}

void ShooterDemo::FillLookAndMove(const InputState& input, const ActionState& actions,
                                  bool allow_keyboard, bool allow_mouse, f32 dt) {
  auto* intent = ctx_.world->Get<character::CharacterIntent>(player_);
  auto* state = ctx_.world->Get<character::CharacterState>(player_);
  if (!intent || !state) return;

  f32 forward = 0;
  f32 right = 0;
  bool sprint = false;
  bool crouch = false;
  bool jump = false;
  f32 yaw_delta = 0;
  f32 pitch_delta = 0;

  if (allow_keyboard) {
    forward = -actions.axis(Axis::kMoveY);
    right = actions.axis(Axis::kMoveX);
    sprint = actions.down(Action::kSprint);
    crouch = actions.down(Action::kSneak);
    jump = actions.pressed(Action::kJump);
  }
  if (mouse_captured_ && allow_mouse) {
    // Aiming down sights slows the look the same way the fov narrows, so the
    // scope is usable.
    f32 sensitivity = look_sensitivity_;
    if (auto* loadout = ctx_.world->Get<combat::Loadout>(player_)) {
      sensitivity *= combat::AimFovScale(*loadout, catalog_);
    }
    yaw_delta = input.mouse_dx * sensitivity;
    pitch_delta = -input.mouse_dy * sensitivity * (invert_pitch_ ? -1.0f : 1.0f);
    yaw_delta += actions.axis(Axis::kLookX) * 2.4f * dt;
    pitch_delta -= actions.axis(Axis::kLookY) * 2.4f * dt * (invert_pitch_ ? -1.0f : 1.0f);
  }

  const Quat heading = HeadingQuat(state->yaw);
  Vec3 move = Rotate(heading, {0, 0, -1}) * forward + Rotate(heading, {1, 0, 0}) * right;
  const f32 length = Length(move);
  if (length > 1.0f) move = move * (1.0f / length);
  intent->move = move;
  intent->gait = sprint ? character::CharacterGait::kSprint : character::CharacterGait::kRun;
  intent->crouch = crouch;
  if (jump) intent->jump = true;
  // Accumulate: FillLookAndMove runs once per render frame, StepCharacters on
  // the fixed accumulator.
  intent->look_yaw_delta += yaw_delta;
  intent->look_pitch_delta += pitch_delta;
}

void ShooterDemo::FillWeaponIntent() {
  ecs::World& world = *ctx_.world;
  auto* intent = world.Get<combat::WeaponIntent>(player_);
  auto* state = world.Get<character::CharacterState>(player_);
  if (!intent || !state) return;

  // Rounds leave from the eye along the resolved camera, which is what the
  // player is actually aiming with.
  intent->origin = cam_eye_;
  intent->direction = Rotate(cam_orientation_, Vec3{0, 0, -1});
  intent->speed = Length(Vec3{state->velocity.x, 0, state->velocity.z});
  intent->airborne = !state->grounded;
  intent->crouched = state->stance == character::CharacterStance::kCrouching;
  intent->reload = pending_reload_;
  intent->switch_to = pending_switch_;
  pending_reload_ = false;
  pending_switch_ = -1;

  if (!autofire_) return;
  // Capture hook: aim at the nearest target that is still up and hold down.
  const scene::Transform* best = nullptr;
  f32 best_distance = 1e9f;
  for (const Target& target : targets_) {
    const combat::Health* health = world.Get<combat::Health>(target.entity);
    const scene::Transform* transform = world.Get<scene::Transform>(target.entity);
    if (!health || health->dead || !transform) continue;
    const Vec3 position{transform->position[0], transform->position[1], transform->position[2]};
    const f32 distance = Length(position - cam_eye_);
    if (distance < best_distance) {
      best_distance = distance;
      best = transform;
    }
  }
  intent->trigger = true;
  if (best) {
    const Vec3 aim{best->position[0], best->position[1] + 0.95f, best->position[2]};
    intent->direction = Normalize(aim - cam_eye_);
  }
}

void ShooterDemo::StepTargets(f32 dt) {
  ecs::World& world = *ctx_.world;
  physics::PhysicsWorld& phys = *ctx_.physics;
  const f32 identity[4] = {0, 0, 0, 1};

  for (Target& target : targets_) {
    combat::Health* health = world.Get<combat::Health>(target.entity);
    if (!health) continue;

    if (health->dead) {
      target.respawn_timer -= dt;
      if (target.respawn_timer <= 0) {
        health->hp = health->max_hp;
        health->dead = false;
      }
    }

    target.strafe_phase += dt * 0.7f;
    Vec3 position = target.home;
    if (target.strafe_span > 0) {
      position.x += std::sin(target.strafe_phase) * target.strafe_span;
    }
    // A downed target drops through the floor and pops back up on respawn,
    // which also takes its hitboxes out of the line of fire.
    if (health->dead) position.y -= 2.2f;

    phys.MoveBodyKinematic(target.torso, position + Vec3{0, 0.95f, 0}, identity, dt);
    phys.MoveBodyKinematic(target.head, position + Vec3{0, 1.72f, 0}, identity, dt);
    if (auto* transform = world.Get<scene::Transform>(target.entity)) {
      transform->position[0] = position.x;
      transform->position[1] = position.y;
      transform->position[2] = position.z;
    }
  }
}

void ShooterDemo::DrainEvents() {
  rounds_ += static_cast<u32>(events_.shots.size());

  // Tracers leave the muzzle, not the eye: a line drawn from the camera to the
  // impact is exactly end-on and draws as a single pixel.
  const Vec3 muzzle = cam_eye_ + Rotate(cam_orientation_, Vec3{0.18f, -0.13f, 0}) +
                      Rotate(cam_orientation_, Vec3{0, 0, -1}) * 0.45f;

  for (const combat::ImpactEvent& impact : events_.impacts) {
    tracers_.push_back(Tracer{muzzle, impact.position, 0.05f});
    // Bullet holes lie flat on the surface they hit.
    const Vec3 normal = Normalize(impact.normal);
    const Quat rotation = QuatBetween({0, 0, 1}, normal);
    marks_.push_back(Mark{MakeTranslation(impact.position + normal * 0.01f) *
                              MakeFromQuat(rotation) * ScaleMat({0.05f, 0.05f, 0.01f}),
                          6.0f});
  }

  for (const combat::DamageEvent& damage : events_.damage) {
    if (damage.instigator == player_) {
      ++hits_;
      hitmarker_ = 0.12f;
      char text[32];
      std::snprintf(text, sizeof(text), "%d", static_cast<int>(damage.applied + 0.5f));
      const u32 color = damage.zone == combat::HitZone::kHead ? 0xffd24a : 0xffffff;
      popups_.push_back(Popup{damage.position, text, Rgba(color, 1.0f), 1.0f});
    }
    if (damage.killed) {
      ++kills_;
      popups_.push_back(Popup{damage.position + Vec3{0, 0.4f, 0}, "DOWN", Rgba(0xff5a4a, 1.0f),
                              1.6f});
      for (Target& target : targets_) {
        if (target.entity == damage.target) target.respawn_timer = 3.0f;
      }
    }
  }

  for (const combat::ExplosionEvent& blast : events_.explosions) {
    marks_.push_back(Mark{MakeTranslation(blast.position) * MakeScale(blast.radius * 0.5f), 0.18f});
    popups_.push_back(Popup{blast.position, "BOOM", Rgba(0xffa030, 1.0f), 0.7f});
  }

  events_.Clear();
}

void ShooterDemo::AgePresentation(f32 dt) {
  AgeOut(tracers_, dt);
  AgeOut(marks_, dt);
  AgeOut(popups_, dt);
  hitmarker_ = std::max(0.0f, hitmarker_ - dt);
  for (Popup& popup : popups_) popup.position.y += dt * 0.35f;
}

void ShooterDemo::Update(f32 dt, const InputState& input, const ActionState& actions,
                         bool allow_keyboard, bool allow_mouse) {
  if (!player_ || dt <= 0) return;
  ecs::World& world = *ctx_.world;
  physics::PhysicsWorld& phys = *ctx_.physics;

  if (allow_keyboard && input.key_pressed(Key::kTab)) mouse_captured_ = !mouse_captured_;
  if (allow_keyboard && mouse_captured_) {
    if (input.key_pressed(Key::kR)) pending_reload_ = true;
    if (input.key_pressed(Key::k1)) pending_switch_ = 0;
    if (input.key_pressed(Key::k2)) pending_switch_ = 1;
    if (input.key_pressed(Key::k3)) pending_switch_ = 2;
    if (input.key_pressed(Key::k4)) pending_switch_ = 3;
    if (input.key_pressed(Key::kG)) ResetPlayer();
    if (input.key_pressed(Key::kM)) show_panel_ = !show_panel_;
    if (input.wheel != 0) {
      if (auto* loadout = world.Get<combat::Loadout>(player_)) {
        const int count = std::max<int>(loadout->count, 1);
        const int step = input.wheel > 0 ? 1 : -1;
        pending_switch_ = static_cast<i8>((loadout->active + count + step) % count);
      }
    }
  }

  const bool firing = mouse_captured_ && allow_mouse && input.button(MouseButton::kLeft);
  const bool aiming = mouse_captured_ && allow_mouse && input.button(MouseButton::kRight);
  if (auto* intent = world.Get<combat::WeaponIntent>(player_)) {
    intent->trigger = firing;
    intent->aim = aiming;
  }

  const f32 fixed = ShooterFixedStep();
  FillLookAndMove(input, actions, allow_keyboard, allow_mouse, dt);

  sim_accum_ += std::min(dt, 0.25f);
  int steps = 0;
  while (sim_accum_ >= fixed) {
    sim_accum_ -= fixed;
    ++steps;
  }

  for (int i = 0; i < steps; ++i) {
    // Recoil first, folded into the same look deltas the mouse writes, so the
    // player fights it with the mouse instead of against the camera rig.
    combat::StepViewRecoil(world, fixed);
    if (auto* recoil = world.Get<combat::ViewRecoil>(player_)) {
      if (auto* intent = world.Get<character::CharacterIntent>(player_)) {
        intent->look_yaw_delta += recoil->view_yaw_delta;
        intent->look_pitch_delta += recoil->view_pitch_delta;
      }
    }

    character::StepCharacters(world, phys, fixed);
    character::SyncCharacterCameraAnchors(world);
    scene::BuildCameraRigs(world, fixed);
    scene::PrepareCameraRigConstraints(world, fixed);
    character::AnswerCameraObstructions(world, phys);
    scene::ResolveCameraRigs(world, fixed);
    scene::ResolveCameraStacks(world, fixed);

    // The camera is resolved, so the muzzle knows where it is pointing.
    if (auto* output = world.Get<scene::CameraOutput>(camera_output_)) {
      cam_eye_ = output->view.position;
      cam_orientation_ = output->view.orientation;
      cam_target_ = cam_eye_ + Rotate(cam_orientation_, Vec3{0, 0, -1});
      cam_base_fov_ = output->view.lens.fov_y;
      cam_valid_ = output->valid;
    }

    FillWeaponIntent();
    combat::StepWeapons(world, phys, catalog_, registry_, events_, fixed);
    combat::StepProjectiles(world, phys, registry_, events_, fixed);
    combat::StepHealth(world, fixed);
    combat::StepViewmodels(world, fixed);

    // Kill events arm a target's respawn timer before its lifecycle advances.
    // Doing this in the opposite order revives a newly killed target immediately
    // because its timer still has the zero-initialized value.
    DrainEvents();
    StepTargets(fixed);
  }

  // Aiming narrows the fov; the character rig owns the lens, so fold the
  // weapon's zoom in after it resolved.
  f32 fov_scale = 1.0f;
  if (auto* loadout = world.Get<combat::Loadout>(player_))
    fov_scale = combat::AimFovScale(*loadout, catalog_);
  // Always derive the displayed FOV from the unscaled camera lens. At refresh
  // rates above the fixed-step rate, some render frames run no simulation step;
  // multiplying cam_fov_ in place on those frames compounds the ADS zoom.
  cam_fov_ = cam_base_fov_ * fov_scale;

  AgePresentation(dt);
}

void ShooterDemo::EmitViewmodel(render::FrameView& view) {
  const combat::Viewmodel* viewmodel = ctx_.world->Get<combat::Viewmodel>(player_);
  const combat::Loadout* loadout = ctx_.world->Get<combat::Loadout>(player_);
  if (!viewmodel || !loadout || !cam_valid_) return;
  const combat::WeaponState* weapon = combat::ActiveWeapon(*loadout);
  if (!weapon) return;

  // View-space -> world: the camera basis, offset by the pose the viewmodel
  // system produced (sway, bob, aim pull-in, per-shot punch).
  const Vec3 right = Rotate(cam_orientation_, {1, 0, 0});
  const Vec3 up = Rotate(cam_orientation_, {0, 1, 0});
  const Vec3 forward = Rotate(cam_orientation_, {0, 0, -1});
  const Vec3 rest{0.22f, -0.19f, 0.42f};
  const Vec3 offset = rest + viewmodel->offset;
  const Vec3 origin = cam_eye_ + right * offset.x + up * offset.y + forward * offset.z;

  const Quat roll = QuatFromAxisAngle(forward, viewmodel->roll);
  const Mat4 rotation = MakeFromQuat(roll * cam_orientation_);
  const u32 tint = 0x2e3238;

  render::DrawItem body{};
  body.mesh = cube_mesh_;
  body.transform = MakeTranslation(origin) * rotation * ScaleMat({0.05f, 0.07f, 0.22f});
  body.prev_transform = body.transform;
  body.tint = tint;
  view.draws.push_back(body);

  render::DrawItem barrel{};
  barrel.mesh = cube_mesh_;
  barrel.transform = MakeTranslation(origin + forward * 0.2f + up * 0.02f) * rotation *
                     ScaleMat({0.022f, 0.022f, 0.2f});
  barrel.prev_transform = barrel.transform;
  barrel.tint = 0x1a1d21;
  view.draws.push_back(barrel);
}

void ShooterDemo::Emit(f32 dt, render::FrameView& view) {
  (void)dt;
  ecs::World& world = *ctx_.world;
  physics::PhysicsWorld& phys = *ctx_.physics;

  if (cam_valid_) {
    view.camera.eye = cam_eye_;
    view.camera.target = cam_target_;
    view.camera.fov_y = cam_fov_;
  }

  for (const Prop& prop : props_) {
    render::DrawItem draw{};
    draw.mesh = cube_mesh_;
    draw.transform = prop.transform;
    draw.prev_transform = prop.transform;
    draw.tint = prop.tint;
    view.draws.push_back(draw);
  }

  for (const Crate& crate : crates_) {
    Vec3 position;
    f32 rotation[4];
    if (!phys.GetBodyTransform(crate.body, &position, rotation)) continue;
    render::DrawItem draw{};
    draw.mesh = cube_mesh_;
    draw.transform = MakeTranslation(position) *
                     MakeFromQuat(rotation[0], rotation[1], rotation[2], rotation[3]) *
                     ScaleMat(crate.half_extent * 2.0f);
    draw.prev_transform = draw.transform;
    draw.tint = crate.tint;
    view.draws.push_back(draw);
  }

  for (const Target& target : targets_) {
    const combat::Health* health = world.Get<combat::Health>(target.entity);
    const scene::Transform* transform = world.Get<scene::Transform>(target.entity);
    if (!health || !transform) continue;
    const Vec3 feet{transform->position[0], transform->position[1], transform->position[2]};
    const f32 wounded = health->max_hp > 0 ? health->hp / health->max_hp : 0.0f;
    // The torso's red dims as it takes damage, so a half-dead target reads at a
    // glance; the head stays bright, because it is the thing worth hitting.
    const u32 red = static_cast<u32>(60.0f + 150.0f * std::clamp(wounded, 0.0f, 1.0f));
    const u32 torso_tint = health->dead ? 0x2a2c30 : (red << 16) | 0x1c1c;
    render::DrawItem torso{};
    torso.mesh = cube_mesh_;
    torso.transform = BoxTransform(feet + Vec3{0, 0.95f, 0}, {0.3f, 0.55f, 0.18f});
    torso.prev_transform = torso.transform;
    torso.tint = torso_tint;
    view.draws.push_back(torso);

    render::DrawItem head{};
    head.mesh = cube_mesh_;
    head.transform = BoxTransform(feet + Vec3{0, 1.72f, 0}, {0.13f, 0.13f, 0.13f});
    head.prev_transform = head.transform;
    head.tint = health->dead ? 0x2a2c30 : 0xe0c040;
    view.draws.push_back(head);
  }

  // Grenades in flight.
  world.Each<combat::Projectile>([&](ecs::Entity, combat::Projectile& round) {
    render::DrawItem draw{};
    draw.mesh = sphere_mesh_;
    draw.transform = MakeTranslation(round.position) *
                     MakeScale(std::max(round.radius, 0.08f) * 2.0f);
    draw.prev_transform = draw.transform;
    draw.tint = 0x30d040;
    view.draws.push_back(draw);
  });

  for (const Mark& mark : marks_) {
    render::DrawItem draw{};
    draw.mesh = cube_mesh_;
    draw.transform = mark.transform;
    draw.prev_transform = mark.transform;
    draw.tint = 0x141518;
    view.draws.push_back(draw);
  }

  EmitViewmodel(view);

  lines_.clear();
  for (const Tracer& tracer : tracers_) {
    lines_.push_back(render::DebugLine{tracer.from, tracer.to, Rgba(0xffd070, tracer.life / 0.05f)});
  }
  view.debug_lines = std::span<const render::DebugLine>(lines_.begin(), lines_.size());

  for (const Popup& popup : popups_) {
    render::WorldText text;
    text.position = popup.position;
    text.text = popup.text;
    text.size = 0.32f;
    text.rgba = popup.rgba;
    text.overlay = false;
    view.world_texts.push_back(text);
  }

  DrawHud();
}

void ShooterDemo::DrawHud() {
#if defined(RX_HAS_IMGUI)
  if (ImGui::GetCurrentContext() == nullptr) return;
  ecs::World& world = *ctx_.world;
  auto* loadout = world.Get<combat::Loadout>(player_);
  auto* intent = world.Get<combat::WeaponIntent>(player_);
  auto* health = world.Get<combat::Health>(player_);
  if (!loadout || !intent) return;
  combat::WeaponState* weapon = combat::ActiveWeapon(*loadout);
  const combat::WeaponDef* def = weapon ? catalog_.Find(weapon->def) : nullptr;

  // --- crosshair: the cone the next round actually leaves in ----------------
  ImDrawList* draw = ImGui::GetForegroundDrawList();
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const ImVec2 middle{size.x * 0.5f, size.y * 0.5f};
  if (def && weapon && cam_fov_ > 0) {
    const f32 spread = combat::EffectiveSpread(*def, *weapon, *intent);
    const f32 pixels = std::clamp(
        std::tan(spread) / std::tan(cam_fov_ * 0.5f) * size.y * 0.5f, 3.0f, size.y * 0.45f);
    const u32 color = IM_COL32(235, 240, 245, 210);
    const f32 arm = std::max(4.0f, pixels * 0.35f);
    draw->AddLine({middle.x - pixels - arm, middle.y}, {middle.x - pixels, middle.y}, color, 1.6f);
    draw->AddLine({middle.x + pixels, middle.y}, {middle.x + pixels + arm, middle.y}, color, 1.6f);
    draw->AddLine({middle.x, middle.y - pixels - arm}, {middle.x, middle.y - pixels}, color, 1.6f);
    draw->AddLine({middle.x, middle.y + pixels}, {middle.x, middle.y + pixels + arm}, color, 1.6f);
    draw->AddCircleFilled(middle, 1.5f, color);
  }
  if (hitmarker_ > 0) {
    const u32 color = IM_COL32(255, 90, 70, static_cast<int>(255 * (hitmarker_ / 0.12f)));
    for (int sx = -1; sx <= 1; sx += 2) {
      for (int sy = -1; sy <= 1; sy += 2) {
        draw->AddLine({middle.x + sx * 6.0f, middle.y + sy * 6.0f},
                      {middle.x + sx * 14.0f, middle.y + sy * 14.0f}, color, 2.0f);
      }
    }
  }

  // --- ammo / health --------------------------------------------------------
  ImGui::SetNextWindowPos({size.x - 250.0f, size.y - 120.0f}, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.35f);
  if (ImGui::Begin("hud", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                       ImGuiWindowFlags_NoNav)) {
    static const char* kNames[4] = {"rifle", "shotgun", "marksman", "launcher"};
    const char* name = loadout->active < 4 ? kNames[loadout->active] : "weapon";
    if (weapon && def) {
      ImGui::Text("%-9s %3u / %-3u", name, weapon->ammo, weapon->reserve);
      if (weapon->reload_timer > 0) ImGui::TextUnformatted("reloading");
      else if (loadout->swap_timer > 0) ImGui::TextUnformatted("switching");
      else ImGui::NewLine();
    }
    if (health) {
      ImGui::Text("hp %3d", static_cast<int>(health->hp + 0.5f));
      ImGui::SameLine();
      ImGui::ProgressBar(health->max_hp > 0 ? health->hp / health->max_hp : 0.0f, {110, 12}, "");
    }
    ImGui::Text("kills %u   hits %u / %u", kills_, hits_, rounds_);
  }
  ImGui::End();

  if (!show_panel_) return;

  // --- live weapon tuning ---------------------------------------------------
  ImGui::SetNextWindowSize({330, 0}, ImGuiCond_FirstUseEver);
  if (ImGui::Begin("shooter (M hides)")) {
    ImGui::TextWrapped(
        "LMB fire, RMB aim, R reload, 1-4 / wheel weapons, G reset, Tab cursor.");
    ImGui::Separator();
    ImGui::SliderFloat("look sensitivity", &look_sensitivity_, 0.0005f, 0.008f, "%.4f");
    ImGui::Checkbox("invert pitch", &invert_pitch_);

    if (def && weapon) {
      // The catalog hands out const defs, so edits go through a copy that is
      // re-registered under the same id: exactly what a data reload does.
      combat::WeaponDef edited = *def;
      bool changed = false;
      ImGui::SeparatorText("rate + damage");
      changed |= ImGui::SliderFloat("rpm", &edited.rpm, 30.0f, 1200.0f, "%.0f");
      changed |= ImGui::SliderFloat("damage", &edited.damage, 1.0f, 150.0f, "%.1f");
      changed |= ImGui::SliderFloat("falloff start", &edited.falloff_start, 0.0f, 150.0f, "%.0f m");
      changed |= ImGui::SliderFloat("falloff end", &edited.falloff_end, 0.0f, 300.0f, "%.0f m");
      changed |= ImGui::SliderFloat("falloff floor", &edited.falloff_min_scale, 0.0f, 1.0f, "%.2f");
      ImGui::SeparatorText("accuracy");
      changed |= ImGui::SliderFloat("spread min", &edited.spread_min, 0.0f, 0.06f, "%.4f rad");
      changed |= ImGui::SliderFloat("spread max", &edited.spread_max, 0.0f, 0.2f, "%.4f rad");
      changed |= ImGui::SliderFloat("bloom / shot", &edited.spread_per_shot, 0.0f, 1.0f, "%.2f");
      changed |= ImGui::SliderFloat("bloom decay", &edited.spread_decay, 0.0f, 6.0f, "%.2f /s");
      changed |= ImGui::SliderFloat("ads spread", &edited.spread_ads_scale, 0.0f, 1.0f, "%.2f");
      ImGui::SeparatorText("recoil + handling");
      changed |= ImGui::SliderFloat("recoil pitch", &edited.recoil_pitch, 0.0f, 0.12f, "%.4f rad");
      changed |= ImGui::SliderFloat("recoil yaw", &edited.recoil_yaw, -0.03f, 0.03f, "%.4f rad");
      changed |= ImGui::SliderFloat("recoil jitter", &edited.recoil_yaw_variance, 0.0f, 0.03f,
                                    "%.4f rad");
      changed |= ImGui::SliderFloat("ads time", &edited.ads_time, 0.02f, 0.8f, "%.2f s");
      changed |= ImGui::SliderFloat("ads fov", &edited.ads_fov_scale, 0.2f, 1.0f, "%.2f");
      changed |= ImGui::SliderFloat("reload", &edited.reload_time, 0.2f, 5.0f, "%.2f s");
      ImGui::SeparatorText("penetration + blast");
      int penetrations = static_cast<int>(edited.max_penetrations);
      if (ImGui::SliderInt("pass-throughs", &penetrations, 0, 4)) {
        edited.max_penetrations = static_cast<u32>(penetrations);
        changed = true;
      }
      changed |= ImGui::SliderFloat("thickness budget", &edited.penetration, 0.0f, 1.0f, "%.2f m");
      changed |= ImGui::SliderFloat("blast radius", &edited.blast_radius, 0.0f, 12.0f, "%.1f m");
      changed |= ImGui::SliderFloat("blast damage", &edited.blast_damage, 0.0f, 300.0f, "%.0f");
      if (changed) catalog_.Register(weapon->def, edited);

      ImGui::SeparatorText("live");
      ImGui::Text("bloom %.2f   ads %.2f   cooldown %.3f s", weapon->bloom, weapon->ads,
                  weapon->cooldown);
      ImGui::Text("spread %.4f rad", combat::EffectiveSpread(edited, *weapon, *intent));
      if (auto* recoil = world.Get<combat::ViewRecoil>(player_)) {
        ImGui::Text("recoil pending %.4f  recoverable %.4f", recoil->pending_pitch,
                    recoil->recoverable_pitch);
        ImGui::SliderFloat("recovery", &recoil->recovery_fraction, 0.0f, 1.0f, "%.2f");
        ImGui::SliderFloat("recovery half-life", &recoil->recovery_half_life, 0.02f, 1.0f,
                           "%.2f s");
      }
    }
    if (auto* viewmodel = world.Get<combat::Viewmodel>(player_)) {
      ImGui::SeparatorText("viewmodel");
      ImGui::SliderFloat("sway", &viewmodel->sway_scale, 0.0f, 0.05f, "%.3f");
      ImGui::SliderFloat("bob", &viewmodel->bob_amplitude, 0.0f, 0.08f, "%.3f m");
      ImGui::SliderFloat("punch", &viewmodel->punch_scale, 0.0f, 0.1f, "%.3f m");
    }
    ImGui::SeparatorText("range");
    ImGui::Text("targets %zu   registered hitboxes %zu", static_cast<size_t>(targets_.size()),
                static_cast<size_t>(registry_.size()));
    if (ImGui::Button("reset player + ammo")) ResetPlayer();
  }
  ImGui::End();
#endif
}

}  // namespace rx
