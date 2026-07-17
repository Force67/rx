#include "demo_puppet.h"

#include <cmath>
#include <span>
#include <string>

#include "asset/asset_id.h"
#include "asset/primitives.h"
#include "core/log.h"
#include "scene/components.h"

namespace rx {
namespace {

using locomotion::BodyPart;
using locomotion::ControlMode;
using locomotion::kBodyPartCount;

// render::DebugLine wants 0xAABBGGRR, matching the nav overlay's PackColor.
u32 PackColor(f32 r, f32 g, f32 b, f32 a = 1.0f) {
  auto to8 = [](f32 v) { return static_cast<u32>((v < 0 ? 0 : (v > 1 ? 1 : v)) * 255.0f); };
  return (to8(a) << 24) | (to8(b) << 16) | (to8(g) << 8) | to8(r);
}

u32 ModeColor(ControlMode mode) {
  switch (mode) {
    case ControlMode::kStable: return PackColor(0.20f, 0.90f, 0.30f);         // green
    case ControlMode::kCorrectiveStep: return PackColor(0.95f, 0.85f, 0.15f); // yellow
    case ControlMode::kControlledFall: return PackColor(0.95f, 0.20f, 0.15f); // red
    case ControlMode::kGrounded: return PackColor(0.65f, 0.25f, 0.85f);       // purple
    case ControlMode::kRecovering: return PackColor(0.20f, 0.55f, 1.00f);     // blue
  }
  return 0xffffffffu;
}

const char* ModeName(ControlMode mode) {
  switch (mode) {
    case ControlMode::kStable: return "stable";
    case ControlMode::kCorrectiveStep: return "corrective-step";
    case ControlMode::kControlledFall: return "controlled-fall";
    case ControlMode::kGrounded: return "grounded";
    case ControlMode::kRecovering: return "recovering";
  }
  return "?";
}

void PushLine(base::Vector<render::DebugLine>& out, const Vec3& a, const Vec3& b, u32 rgba) {
  out.push_back(render::DebugLine{a, b, rgba});
}

// A small 3D cross (three axis segments) marking a point.
void PushCross(base::Vector<render::DebugLine>& out, const Vec3& p, f32 s, u32 rgba) {
  PushLine(out, {p.x - s, p.y, p.z}, {p.x + s, p.y, p.z}, rgba);
  PushLine(out, {p.x, p.y - s, p.z}, {p.x, p.y + s, p.z}, rgba);
  PushLine(out, {p.x, p.y, p.z - s}, {p.x, p.y, p.z + s}, rgba);
}

// Horizontal ring on the ground plane at `p`, radius r, `segs` segments.
void PushRing(base::Vector<render::DebugLine>& out, const Vec3& p, f32 r, u32 rgba, u32 segs = 16) {
  Vec3 prev{p.x + r, p.y, p.z};
  for (u32 i = 1; i <= segs; ++i) {
    const f32 a = 6.2831853f * static_cast<f32>(i) / static_cast<f32>(segs);
    Vec3 cur{p.x + std::cos(a) * r, p.y, p.z + std::sin(a) * r};
    PushLine(out, prev, cur, rgba);
    prev = cur;
  }
}

// A planar arrow from `from` toward `from + dir`, with a short two-line head.
void PushArrow(base::Vector<render::DebugLine>& out, const Vec3& from, const Vec3& dir, u32 rgba) {
  const Vec3 tip{from.x + dir.x, from.y + dir.y, from.z + dir.z};
  PushLine(out, from, tip, rgba);
  const f32 len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
  if (len < 1e-3f) return;
  const Vec3 back{-dir.x / len, 0, -dir.z / len};
  const Vec3 side{back.z, 0, -back.x};  // perpendicular in the ground plane
  const f32 h = 0.09f;
  PushLine(out, tip, {tip.x + (back.x + side.x) * h, tip.y, tip.z + (back.z + side.z) * h}, rgba);
  PushLine(out, tip, {tip.x + (back.x - side.x) * h, tip.y, tip.z + (back.z - side.z) * h}, rgba);
}

}  // namespace

PuppetDemo::PuppetDemo(EngineContext& ctx) : ctx_(ctx) {}

void PuppetDemo::BuildArena() {
  physics::PhysicsWorld& phys = *ctx_.physics;

  asset::Material mat;
  mat.id = asset::MakeAssetId("puppet/arena_mat");
  mat.base_color_factor[0] = 0.34f;
  mat.base_color_factor[1] = 0.36f;
  mat.base_color_factor[2] = 0.33f;
  mat.roughness_factor = 0.95f;
  mat.metallic_factor = 0;
  if (!ctx_.config->headless) ctx_.renderer->UploadMaterial(mat);

  int tag = 0;
  // Renders a box (its top is the walkable surface) and mirrors it as a static
  // collider. `rot` is the render/physics rotation quaternion (xyzw).
  auto solid = [&](const Vec3& center, f32 hx, f32 hy, f32 hz, const f32 rot[4], u32 tint) {
    asset::Mesh box =
        asset::MakeBox(hx, hy, hz, asset::MakeAssetId("puppet/box_" + std::to_string(tag++)));
    box.lods[0].submeshes.push_back({0, static_cast<u32>(box.lods[0].indices.size()), mat.id});
    if (!ctx_.config->headless) ctx_.renderer->UploadMesh(box);
    ecs::Entity e = ctx_.world->Create();
    scene::Transform t;
    t.position[0] = center.x;
    t.position[1] = center.y;
    t.position[2] = center.z;
    for (int i = 0; i < 4; ++i) t.rotation[i] = rot[i];
    ctx_.world->Add(e, t);
    ctx_.world->Add(e, scene::Renderable{box.id});
    ctx_.world->Add(e, scene::Tint{tint});
    if (rot[0] == 0 && rot[1] == 0 && rot[2] == 0) {
      phys.AddStaticBox(center, {hx, hy, hz});  // axis-aligned fast path
    } else {
      phys.AddStaticMesh(box, center, rot, 1.0f);  // rotated: bake the mesh collider
    }
  };
  const f32 kIdentity[4] = {0, 0, 0, 1};

  // Flat floor: 24 x 24 m, top surface at y = 0.
  solid({0, -0.5f, 0}, 12.0f, 0.5f, 12.0f, kIdentity, 0x3a3d38);

  // A shallow ~15 deg ramp off to the +X/+Z corner (scenery + a slope to probe).
  const f32 ramp_deg = 15.0f * 3.14159265f / 180.0f;
  const f32 rh = std::sin(ramp_deg * 0.5f), rw = std::cos(ramp_deg * 0.5f);
  const f32 ramp_rot[4] = {rh, 0, 0, rw};  // tilt about +X
  solid({3.0f, 0.35f, 3.0f}, 1.6f, 0.08f, 2.0f, ramp_rot, 0x45483f);

  // A 0.15 m curb / step, top at y = 0.15.
  solid({2.6f, 0.075f, -1.2f}, 1.0f, 0.075f, 0.7f, kIdentity, 0x4a4d42);

  // Scattered boxes as graybox obstacles / scale reference.
  const f32 boxes[][4] = {
      {-3.2f, 0.25f, 2.4f, 0.25f}, {3.6f, 0.20f, -3.4f, 0.20f},
      {-2.4f, 0.30f, -3.0f, 0.30f}, {4.2f, 0.15f, 1.0f, 0.15f},
  };
  for (const auto& b : boxes) {
    solid({b[0], b[1], b[2]}, b[3], b[1], b[3], kIdentity, 0x52564a);
  }
}

void PuppetDemo::BuildProxies() {
  const locomotion::BipedRig& rig = controller_.rig();

  asset::Material mat;
  mat.id = asset::MakeAssetId("puppet/proxy_mat");
  mat.base_color_factor[0] = 0.75f;
  mat.base_color_factor[1] = 0.75f;
  mat.base_color_factor[2] = 0.75f;
  mat.roughness_factor = 0.55f;
  mat.metallic_factor = 0;
  if (!ctx_.config->headless) ctx_.renderer->UploadMaterial(mat);

  // Box half-extents per body, aligned to each capsule's long axis (limbs and
  // torso along local Y, the pelvis laterally along local X). Derived from the
  // rig's public geometry -- a graybox proxy, not the exact collision capsule.
  const f32 hw = params_.hip_width * 0.5f;
  const f32 torso_half = 0.19f * params_.body_height;
  auto ext = [&](BodyPart part) -> Vec3 {
    switch (part) {
      case BodyPart::kPelvis: return {hw + 0.03f, 0.09f, 0.11f};
      case BodyPart::kTorso: return {0.16f, torso_half, 0.11f};
      case BodyPart::kHead: return {0.09f, 0.11f, 0.09f};
      case BodyPart::kUpperLegL:
      case BodyPart::kUpperLegR: return {0.06f, rig.upper_leg_length * 0.5f, 0.06f};
      case BodyPart::kLowerLegL:
      case BodyPart::kLowerLegR: return {0.05f, rig.lower_leg_length * 0.5f, 0.05f};
      case BodyPart::kFootL:
      case BodyPart::kFootR: return {0.045f, rig.foot_height * 0.5f, rig.foot_length * 0.5f};
      case BodyPart::kUpperArmL:
      case BodyPart::kUpperArmR: return {0.045f, rig.upper_arm_length * 0.5f, 0.045f};
      case BodyPart::kLowerArmL:
      case BodyPart::kLowerArmR: return {0.04f, rig.lower_arm_length * 0.5f, 0.04f};
      default: return {0.05f, 0.05f, 0.05f};
    }
  };
  auto tint = [](BodyPart part) -> u32 {
    if (part == BodyPart::kPelvis || part == BodyPart::kTorso) return 0xe08040;  // warm core
    if (part == BodyPart::kHead) return 0xd0b090;                                // tan head
    return 0x5590c0;                                                             // steel limbs
  };

  for (u32 i = 0; i < kBodyPartCount; ++i) {
    const BodyPart part = static_cast<BodyPart>(i);
    const Vec3 e = ext(part);
    asset::Mesh box =
        asset::MakeBox(e.x, e.y, e.z, asset::MakeAssetId("puppet/proxy_" + std::to_string(i)));
    box.lods[0].submeshes.push_back({0, static_cast<u32>(box.lods[0].indices.size()), mat.id});
    if (!ctx_.config->headless) ctx_.renderer->UploadMesh(box);
    proxy_[i] = ctx_.world->Create();
    ctx_.world->Add(proxy_[i], scene::Transform{});
    ctx_.world->Add(proxy_[i], scene::Renderable{box.id});
    ctx_.world->Add(proxy_[i], scene::Tint{tint(part)});
  }
}

void PuppetDemo::ResetPuppet() {
  controller_.Destroy();
  if (!controller_.Initialize(ctx_.physics, params_, spawn_feet_, spawn_yaw_)) {
    RX_WARN("puppet demo: controller re-init failed");
  }
  last_mode_ = controller_.mode();
  mode_logged_ = false;
}

void PuppetDemo::Create() {
  BuildArena();

  if (!controller_.Initialize(ctx_.physics, params_, spawn_feet_, spawn_yaw_)) {
    RX_WARN("puppet demo: controller init failed; arena only");
  }
  BuildProxies();

  // The controller must Tick once per fixed step BEFORE the host advances the
  // shared physics world (its "physics" system runs in kSim). kPreSim guarantees
  // that ordering, so the demo never double-steps Jolt.
  ctx_.scheduler->AddSystem(ecs::Stage::kPreSim, "puppet",
                            [this](ecs::World&, f32 dt) { Step(dt); });

  ctx_.camera->set_position({4.0f, 2.4f, 4.0f});
  ctx_.camera->set_yaw_pitch(0.785f, -0.32f);
  ctx_.camera->speed = 4.0f;
  RX_INFO(
      "puppet demo: rx::locomotion ragdoll on a graybox arena. Scripted "
      "stand/walk/turn/stop loop; keys 1=push 2=big-push 3=reset.");
}

locomotion::LocomotionIntent PuppetDemo::ScriptedIntent() const {
  // stand 3s -> walk -Z at 0.6 m/s 8s -> turn 90 deg and walk -X 6s -> stop 3s.
  // 0.6 m/s is the honest stable walking regime for this ragdoll (docs).
  locomotion::LocomotionIntent intent;
  const f32 t = script_time_;
  const f32 kSpeed = 0.6f;
  if (t < 3.0f) {
    intent.desired_facing = {0, 0, -1};
  } else if (t < 11.0f) {
    intent.desired_facing = {0, 0, -1};
    intent.desired_velocity = {0, 0, -kSpeed};
  } else if (t < 17.0f) {
    // Turn gradually from -Z toward -X over 3 s (an abrupt heading snap while
    // walking tips this slow ragdoll over); velocity follows the heading.
    const f32 phi = std::min((t - 11.0f) / 3.0f, 1.0f) * 1.5707963f;
    const Vec3 dir{-std::sin(phi), 0, -std::cos(phi)};
    intent.desired_facing = dir;
    intent.desired_velocity = {dir.x * kSpeed, 0, dir.z * kSpeed};
  } else {
    intent.desired_facing = {-1, 0, 0};
  }
  return intent;
}

void PuppetDemo::Step(f32 dt) {
  if (!controller_.initialized()) return;

  // Consume raw-key requests here, before the physics step this tick.
  if (pending_reset_) {
    pending_reset_ = false;
    pending_small_push_ = pending_big_push_ = false;
    script_time_ = 0;
    RX_INFO("puppet demo: reset");
    ResetPuppet();
  }
  if (pending_small_push_ || pending_big_push_) {
    const f32 mag = pending_big_push_ ? 200.0f : 40.0f;
    pending_small_push_ = pending_big_push_ = false;
    rng_ = rng_ * 1664525u + 1013904223u;
    const f32 a = 6.2831853f * (static_cast<f32>((rng_ >> 8) & 0xffff) / 65535.0f);
    const Vec3 impulse{std::cos(a) * mag, 0, std::sin(a) * mag};
    ctx_.physics->ApplyImpulse(controller_.rig().body[static_cast<u32>(BodyPart::kTorso)], impulse);
    RX_INFO("puppet demo: push {} kg*m/s", mag);
  }

  script_time_ += dt;
  if (script_time_ >= 20.0f) {  // loop: recentre with a clean reset
    script_time_ -= 20.0f;
    ResetPuppet();
  }

  const locomotion::LocomotionIntent intent = ScriptedIntent();
  const locomotion::PhysicalModifiers mods;
  controller_.Tick(intent, mods, dt);

  const ControlMode mode = controller_.mode();
  if (!mode_logged_ || mode != last_mode_) {
    RX_INFO("puppet demo: mode -> {} (t={:.1f}s)", ModeName(mode), script_time_);
    last_mode_ = mode;
    mode_logged_ = true;
  }
}

void PuppetDemo::OnInput(const InputState& input, bool allow_keyboard) {
  if (!allow_keyboard) return;
  const Key keys[3] = {Key::k1, Key::k2, Key::k3};
  for (int i = 0; i < 3; ++i) {
    const bool down = input.key_pressed(keys[i]);
    if (down && !prev_key_[i]) {
      if (i == 0) pending_small_push_ = true;
      if (i == 1) pending_big_push_ = true;
      if (i == 2) pending_reset_ = true;
    }
    prev_key_[i] = down;
  }
}

void PuppetDemo::Emit(f32 dt, render::FrameView& view) {
  (void)dt;
  if (!controller_.initialized() || ctx_.config->headless) return;

  // Pose every body proxy from its live physics transform (visual only, so
  // render cadence is fine).
  const locomotion::BipedRig& rig = controller_.rig();
  for (u32 i = 0; i < kBodyPartCount; ++i) {
    scene::Transform* t = ctx_.world->Get<scene::Transform>(proxy_[i]);
    if (!t) continue;
    Vec3 pos;
    f32 rot[4];
    if (ctx_.physics->GetBodyTransform(rig.body[i], &pos, rot)) {
      t->position[0] = pos.x;
      t->position[1] = pos.y;
      t->position[2] = pos.z;
      for (int k = 0; k < 4; ++k) t->rotation[k] = rot[k];
    }
  }

  if (draw_lines_) EmitDebugLines(view);
}

void PuppetDemo::EmitDebugLines(render::FrameView& view) {
  lines_.clear();
  const locomotion::DebugState& d = controller_.debug();
  const locomotion::CharacterMeasurements& m = controller_.measurements();
  const locomotion::ContactEstimate& c = controller_.contacts();

  // Capture point: a cross coloured by the current control mode.
  PushCross(lines_, d.capture_point, 0.12f, ModeColor(d.mode));
  // A short vertical mast so the capture point reads against the floor.
  PushLine(lines_, {d.capture_point.x, d.capture_point.y, d.capture_point.z},
           {d.capture_point.x, d.capture_point.y + 0.35f, d.capture_point.z}, ModeColor(d.mode));

  // Support region: a ring around the support centre + a line joining the feet.
  const u32 cyan = PackColor(0.15f, 0.85f, 0.85f);
  if (d.support_count > 0) {
    PushRing(lines_, d.support_center, 0.18f, cyan);
    PushCross(lines_, d.support_center, 0.06f, cyan);
  }
  PushLine(lines_, m.foot[0].position, m.foot[1].position, PackColor(0.15f, 0.55f, 0.55f));

  // Per-foot: planned target marker, sole->target line, swing arc while swinging.
  const u32 yellow = PackColor(0.95f, 0.85f, 0.15f);
  const u32 magenta = PackColor(0.90f, 0.30f, 0.85f);
  for (u32 f = 0; f < locomotion::kFootCount; ++f) {
    const Vec3 sole = m.foot[f].position;
    const Vec3 target = d.foot_target[f];
    PushCross(lines_, target, 0.07f, yellow);
    PushLine(lines_, sole, target, PackColor(0.85f, 0.85f, 0.85f));
    if (c.phase[f] == locomotion::FootPhase::kSwinging) {
      // 8-segment parabolic arc from the sole to the target (apex = step_height).
      Vec3 prev = sole;
      for (u32 s = 1; s <= 8; ++s) {
        const f32 u = static_cast<f32>(s) / 8.0f;
        const f32 lift = params_.step_height * 4.0f * u * (1.0f - u);
        Vec3 cur{sole.x + (target.x - sole.x) * u, sole.y + (target.y - sole.y) * u + lift,
                 sole.z + (target.z - sole.z) * u};
        PushLine(lines_, prev, cur, magenta);
        prev = cur;
      }
    }
  }

  // Desired vs measured planar velocity, as arrows from the COM (scaled 0.4 s).
  const Vec3 com = d.com_position;
  PushArrow(lines_, com, {d.desired_velocity.x * 0.4f, 0, d.desired_velocity.z * 0.4f},
            PackColor(0.20f, 0.90f, 0.30f));
  PushArrow(lines_, com, {d.measured_velocity.x * 0.4f, 0, d.measured_velocity.z * 0.4f},
            PackColor(0.20f, 0.55f, 1.0f));

  // COM vertical drop to the ground under it.
  physics::PhysicsWorld::RayHit hit;
  f32 ground_y = 0.0f;
  if (ctx_.physics->Raycast({com.x, com.y, com.z}, {0, -1, 0}, com.y + 2.0f, &hit)) {
    ground_y = hit.position.y;
  }
  PushLine(lines_, com, {com.x, ground_y, com.z}, PackColor(0.55f, 0.55f, 0.55f));

  view.debug_lines = std::span<const render::DebugLine>(lines_.begin(), lines_.size());
}

}  // namespace rx
