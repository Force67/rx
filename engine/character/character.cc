#include "character/character.h"

#include <algorithm>
#include <cmath>

#include "ecs/world.h"
#include "scene/camera_rig.h"
#include "scene/components.h"

namespace rx::character {
namespace {

constexpr f32 kTwoPi = 6.28318530717958647692f;

f32 WrapAngle(f32 a) { return std::remainder(a, kTwoPi); }

// Heading quaternion: yaw about -Y so a character's forward (-Z at yaw 0) agrees
// with scene::CameraOrbit's yaw and the camera it drives.
Quat HeadingQuat(f32 yaw) { return QuatFromAxisAngle({0, -1, 0}, yaw); }

f32 MoveTowardScalar(f32 current, f32 target, f32 max_delta) {
  const f32 d = target - current;
  if (std::abs(d) <= max_delta) return target;
  return current + (d > 0 ? max_delta : -max_delta);
}

// Frame-rate-independent exponential approach: closes half the gap every
// `half_life` seconds. Converges without overshoot; half_life <= 0 snaps.
f32 ExpApproach(f32 current, f32 target, f32 half_life, f32 dt) {
  if (half_life <= 0.0f) return target;
  const f32 t = 1.0f - std::exp2(-dt / half_life);
  return current + (target - current) * t;
}

// Same, on the shortest signed angular arc (handles wrap; no overshoot).
f32 ExpApproachAngle(f32 current, f32 target, f32 half_life, f32 dt) {
  const f32 diff = WrapAngle(target - current);
  if (half_life <= 0.0f) return WrapAngle(current + diff);
  const f32 t = 1.0f - std::exp2(-dt / half_life);
  return WrapAngle(current + diff * t);
}

// Heading yaw for a horizontal world direction, inverse of HeadingQuat's forward
// (forward at yaw 0 is -Z, yaw increases toward +X): forward = (sin y, 0, -cos y).
f32 YawFromDir(const Vec3& d) { return std::atan2(d.x, -d.z); }

Vec3 MoveTowardVec(const Vec3& current, const Vec3& target, f32 max_delta) {
  const Vec3 d = target - current;
  const f32 len = Length(d);
  if (len <= max_delta || len < 1e-6f) return target;
  return current + d * (max_delta / len);
}

f32 GaitSpeed(const CharacterMovementSettings& s, CharacterGait gait) {
  switch (gait) {
    case CharacterGait::kWalk:
      return s.walk_speed;
    case CharacterGait::kSprint:
      return s.sprint_speed;
    case CharacterGait::kRun:
      break;
  }
  return s.run_speed;
}

// Jolt cylinder half-height (excludes the two radius hemispheres) for a total
// tip-to-tip capsule height.
f32 CylinderHalf(f32 total_height, f32 radius) {
  return std::max(total_height * 0.5f - radius, 0.01f);
}

Vec3 TransformFeet(const scene::Transform& t) {
  return {t.position[0], t.position[1], t.position[2]};
}

template <typename T>
T& Ensure(ecs::World& world, ecs::Entity e, const T& value = {}) {
  if (T* existing = world.Get<T>(e)) return *existing;
  return world.Add(e, value);
}

template <typename T>
void RemoveIfPresent(ecs::World& world, ecs::Entity e) {
  if (world.Has<T>(e)) world.Remove<T>(e);
}

}  // namespace

void StepCharacters(ecs::World& world, physics::PhysicsWorld& physics, f32 dt) {
  if (!std::isfinite(dt) || dt <= 0) return;

  world.Each<CharacterMovementSettings, CharacterShape, CharacterIntent, CharacterState,
             CharacterBody, scene::Transform>(
      [&](ecs::Entity entity, CharacterMovementSettings& settings, CharacterShape& shape,
          CharacterIntent& intent, CharacterState& state, CharacterBody& body,
          scene::Transform& transform) {
        if (body.id == 0) return;

        // Push stair/slope limits to the controller once.
        if (!body.configured) {
          physics.ConfigureCharacter(body.id, settings.max_slope_angle, settings.step_height);
          body.configured = true;
        }

        // --- Heading + look ---------------------------------------------------
        state.yaw = WrapAngle(state.yaw + intent.look_yaw_delta);
        if (scene::CameraIntent* cam = world.Get<scene::CameraIntent>(entity)) {
          cam->pitch_delta += intent.look_pitch_delta;
        }

        const Vec3 up{0, 1, 0};
        const f32 old_total_half = body.half_height + body.radius;

        Vec3 center{};
        const bool have_center = physics.GetCharacterPosition(body.id, &center);
        const Vec3 feet = have_center ? center - up * old_total_half : TransformFeet(transform);

        // --- Stance resolution ------------------------------------------------
        // Only Standing/Crouching are driven here; reserved stances pass through.
        CharacterStance stance = state.stance;
        if (stance != CharacterStance::kStanding && stance != CharacterStance::kCrouching) {
          stance = CharacterStance::kStanding;
        }
        if (intent.crouch) {
          stance = CharacterStance::kCrouching;  // crouch enter is always allowed
        } else if (stance == CharacterStance::kCrouching) {
          // Crouch exit needs headroom: sweep a sphere up through the gap the
          // standing capsule would reclaim.
          const f32 gap = std::max(shape.standing_height - shape.crouched_height, 0.0f);
          bool clear = true;
          if (gap > 1e-4f) {
            const Vec3 origin = feet + up * (shape.crouched_height - shape.standing_radius);
            physics::PhysicsWorld::RayHit hit;
            clear = !physics.SphereCast(origin, up, gap, shape.standing_radius * 0.9f, &hit);
          }
          if (clear) stance = CharacterStance::kStanding;
        }
        state.stance = stance;

        // --- Crouch blend + capsule resize (feet-planted) --------------------
        const f32 prev_blend = state.crouch_blend;
        const f32 target_blend = stance == CharacterStance::kCrouching ? 1.0f : 0.0f;
        f32 blend =
            MoveTowardScalar(prev_blend, target_blend, shape.crouch_blend_speed * dt);
        blend = std::clamp(blend, 0.0f, 1.0f);

        const f32 radius = std::lerp(shape.standing_radius, shape.crouched_radius, blend);
        const f32 height = std::lerp(shape.standing_height, shape.crouched_height, blend);
        const f32 half_height = CylinderHalf(height, radius);
        const f32 new_total_half = half_height + radius;

        const bool dims_changed = std::abs(new_total_half - old_total_half) > 1e-4f ||
                                  std::abs(radius - body.radius) > 1e-4f;
        if (dims_changed && have_center) {
          const Vec3 new_center = feet + up * new_total_half;
          physics.SetCharacterPosition(body.id, new_center);
          if (physics.SetCharacterShape(body.id, radius, half_height)) {
            body.radius = radius;
            body.half_height = half_height;
          } else {
            // Growing into geometry: keep the old capsule and hold the blend.
            physics.SetCharacterPosition(body.id, center);
            blend = prev_blend;
          }
        }
        state.crouch_blend = blend;
        state.eye_height =
            std::lerp(shape.standing_eye_height, shape.crouched_eye_height, blend);

        // --- Analog move intent ----------------------------------------------
        const Vec3 move_h{intent.move.x, 0, intent.move.z};
        const f32 throttle = std::clamp(Length(move_h), 0.0f, 1.0f);
        const Vec3 dir = throttle > 1e-4f ? Normalize(move_h) : Vec3{0, 0, 0};

        // --- Body facing (turn smoothing) ------------------------------------
        // First person hard-locks the body to the RAW look yaw (no damping ever
        // touches look input); third person eases the facing toward the movement
        // direction, using the faster pivot half-life for near-180 reversals.
        bool face_look = true;
        if (const CharacterViewMode* vm = world.Get<CharacterViewMode>(entity))
          face_look = vm->kind == CharacterViewKind::kFirstPerson;
        if (!state.view_initialized) state.facing_yaw = state.yaw;
        if (face_look) {
          state.facing_yaw = state.yaw;  // locked, raw
        } else if (throttle > 1e-3f) {
          const f32 target_yaw = YawFromDir(dir);
          const f32 gap = std::abs(WrapAngle(target_yaw - state.facing_yaw));
          // Sticky pivot: a reversal past pivot_angle latches the faster rate and
          // holds it through the bulk of the turn (released near completion), so a
          // 180 spins on the spot instead of arcing then stalling.
          if (gap > settings.pivot_angle) state.pivoting = true;
          else if (gap < 0.15f) state.pivoting = false;
          const f32 hl =
              state.pivoting ? settings.pivot_turn_half_life : settings.turn_half_life;
          state.facing_yaw = ExpApproachAngle(state.facing_yaw, target_yaw, hl, dt);
        }  // third person, not moving: hold facing

        // --- Gait target-speed blend (smooth target, snappy accel) -----------
        const f32 raw_gait = stance == CharacterStance::kCrouching
                                 ? settings.crouch_speed
                                 : GaitSpeed(settings, intent.gait);
        const f32 span = std::max(settings.sprint_speed - settings.walk_speed, 0.0f);
        if (settings.speed_blend_time > 0.0f && span > 1e-4f) {
          state.gait_speed =
              MoveTowardScalar(state.gait_speed, raw_gait, (span / settings.speed_blend_time) * dt);
        } else {
          state.gait_speed = raw_gait;
        }
        const Vec3 target_h = dir * (state.gait_speed * throttle);

        // --- Horizontal velocity ---------------------------------------------
        Vec3 horizontal{state.velocity.x, 0, state.velocity.z};
        f32 accel = throttle > 1e-4f ? settings.ground_acceleration : settings.ground_deceleration;
        if (!state.grounded) accel = settings.ground_acceleration * settings.air_control;
        horizontal = MoveTowardVec(horizontal, target_h, std::max(accel, 0.0f) * dt);
        // Crisp stop: no ice-skate tail below the epsilon when input is released.
        if (throttle <= 1e-4f && Length(horizontal) < settings.stop_speed_epsilon)
          horizontal = {0, 0, 0};

        // --- Vertical velocity + jump (buffer + coyote) ----------------------
        const bool grounded_prev = state.grounded;  // last step's result
        if (grounded_prev) state.jump_consumed = false;

        // Jump buffer: a fresh press refills the window; otherwise it drains.
        const bool jump_pressed = intent.jump;
        if (jump_pressed) {
          state.jump_buffer_timer = std::max(settings.jump_buffer_time, 0.0f);
        } else {
          state.jump_buffer_timer = std::max(0.0f, state.jump_buffer_timer - dt);
        }
        const bool jump_wanted = jump_pressed || state.jump_buffer_timer > 0.0f;
        const bool coyote_ok =
            grounded_prev || (!state.jump_consumed && settings.coyote_time > 0.0f &&
                              state.time_since_grounded <= settings.coyote_time);

        f32 vy = state.velocity.y;
        if (grounded_prev && vy < 0) vy = 0;
        if (jump_wanted && coyote_ok) {
          vy = std::sqrt(2.0f * std::max(settings.gravity, 0.0f) *
                         std::max(settings.jump_height, 0.0f));
          state.jump_buffer_timer = 0;
          state.jump_consumed = true;
        }
        vy -= settings.gravity * dt;

        Vec3 velocity{horizontal.x, vy, horizontal.z};

        // --- Drive the controller --------------------------------------------
        Vec3 out_pos = center;
        bool grounded = false;
        physics.MoveCharacterVelocity(body.id, velocity, dt, &out_pos, &grounded, nullptr);

        const bool just_landed = !grounded_prev && grounded;
        const f32 impact_speed = std::max(0.0f, -velocity.y);  // pre-ground-clamp descent speed
        state.grounded = grounded;
        if (grounded && velocity.y < 0) velocity.y = 0;
        state.velocity = velocity;
        state.time_since_grounded = grounded ? 0.0f : state.time_since_grounded + dt;

        const Vec3 out_feet = out_pos - up * (body.half_height + body.radius);
        transform.position[0] = out_feet.x;
        transform.position[1] = out_feet.y;
        transform.position[2] = out_feet.z;
        const Quat q = HeadingQuat(state.facing_yaw);
        transform.rotation[0] = q.x;
        transform.rotation[1] = q.y;
        transform.rotation[2] = q.z;
        transform.rotation[3] = q.w;

        // --- Camera-anchor eye Y: step smoothing + landing dip ---------------
        // Vertical only; horizontal follows the feet 1:1 in SyncCharacterCameraAnchors.
        const f32 raw_eye_y = out_feet.y + state.eye_height;
        if (!state.view_initialized) {
          state.eye_base_y = raw_eye_y;
        } else if (grounded && shape.eye_step_half_life > 0.0f) {
          state.eye_base_y = ExpApproach(state.eye_base_y, raw_eye_y, shape.eye_step_half_life, dt);
        } else {
          state.eye_base_y = raw_eye_y;  // airborne: snap so the jump arc is not damped
        }
        if (just_landed && shape.landing_dip_scale > 0.0f &&
            impact_speed > shape.landing_dip_min_speed) {
          const f32 dip = std::min((impact_speed - shape.landing_dip_min_speed) *
                                       shape.landing_dip_scale,
                                   std::max(shape.landing_dip_max, 0.0f));
          state.landing_dip = std::max(state.landing_dip, dip);
        }
        state.landing_dip = shape.landing_dip_half_life > 0.0f
                                ? state.landing_dip * std::exp2(-dt / shape.landing_dip_half_life)
                                : 0.0f;
        state.anchor_eye_y = state.eye_base_y - state.landing_dip;
        state.view_initialized = true;

        // Consume edge/delta inputs; held state (move/gait/crouch) persists.
        intent.jump = false;
        intent.look_yaw_delta = 0;
        intent.look_pitch_delta = 0;
      });
}

void SyncCharacterCameraAnchors(ecs::World& world) {
  world.Each<CharacterState, scene::CameraAnchor>(
      [&](ecs::Entity entity, CharacterState& state, scene::CameraAnchor& anchor) {
        Vec3 feet = anchor.position;
        if (scene::Transform* t = world.Get<scene::Transform>(entity)) feet = TransformFeet(*t);
        // Horizontal follows the feet 1:1 (raw); the vertical is the step-smoothed,
        // landing-dipped eye Y once StepCharacters has primed it.
        const f32 eye_y = state.view_initialized ? state.anchor_eye_y : feet.y + state.eye_height;
        anchor.position = {feet.x, eye_y, feet.z};
        // Camera heading is the RAW look yaw (never the smoothed body facing).
        anchor.orientation = HeadingQuat(state.yaw);
        anchor.velocity = state.velocity;
        if (state.teleported) {
          ++anchor.revision;
          state.teleported = false;
        }
      });
}

void AnswerCameraObstructions(ecs::World& world, physics::PhysicsWorld& physics) {
  world.Each<scene::CameraObstruction>([&](ecs::Entity, scene::CameraObstruction& o) {
    if (o.request_id == 0 || o.result_request_id == o.request_id) return;  // none / already answered

    const Vec3 delta = o.desired_position - o.origin;
    const f32 distance = Length(delta);
    if (distance < 1e-5f) {
      scene::SetCameraObstructionResult(o, o.request_id, o.desired_position, false);
      return;
    }
    const Vec3 dir = delta * (1.0f / distance);
    physics::PhysicsWorld::RayHit hit;
    if (physics.SphereCast(o.origin, dir, distance, o.radius, &hit)) {
      const f32 safe = std::clamp(hit.distance - o.margin, 0.0f, distance);
      scene::SetCameraObstructionResult(o, o.request_id, o.origin + dir * safe, true);
    } else {
      scene::SetCameraObstructionResult(o, o.request_id, o.desired_position, false);
    }
  });
}

void ApplyCharacterViewMode(ecs::World& world, ecs::Entity entity,
                            const CharacterViewSettings& settings) {
  CharacterViewMode* view = world.Get<CharacterViewMode>(entity);
  if (!view) return;

  // Shared scaffolding: the entity is its own camera-mode + rig + anchor source.
  Ensure<scene::CameraAnchor>(world, entity);
  Ensure<scene::CameraIntent>(world, entity);
  Ensure<scene::CameraRigPose>(world, entity);
  Ensure<scene::CameraMode>(world, entity);
  scene::CameraOrbit& orbit = Ensure<scene::CameraOrbit>(world, entity);
  orbit.space = scene::CameraOrbitSpace::kAnchor;
  orbit.yaw = 0;  // yaw comes from the anchor heading; the orbit owns only pitch

  if (view->kind == CharacterViewKind::kFirstPerson) {
    orbit.min_pitch = -settings.fp_pitch_limit;
    orbit.max_pitch = settings.fp_pitch_limit;
    orbit.pitch = std::clamp(orbit.pitch, orbit.min_pitch, orbit.max_pitch);
    // Eye-anchored, zero lag, no boom or obstruction.
    RemoveIfPresent<scene::CameraLocalOffset>(world, entity);
    RemoveIfPresent<scene::CameraBoom>(world, entity);
    RemoveIfPresent<scene::CameraObstruction>(world, entity);
    RemoveIfPresent<scene::CameraDamping>(world, entity);
    RemoveIfPresent<scene::CameraFraming>(world, entity);
  } else {
    orbit.min_pitch = settings.tp_pitch_min;
    orbit.max_pitch = settings.tp_pitch_max;
    orbit.pitch = std::clamp(orbit.pitch, orbit.min_pitch, orbit.max_pitch);
    scene::CameraBoom& boom = Ensure<scene::CameraBoom>(world, entity);
    boom.distance = std::clamp(boom.distance == 0 ? settings.tp_distance : boom.distance,
                               settings.tp_min_distance, settings.tp_max_distance);
    boom.shoulder_offset = settings.tp_shoulder_offset;
    boom.height_offset = settings.tp_height_offset;
    scene::CameraObstruction& obstruction = Ensure<scene::CameraObstruction>(world, entity);
    obstruction.radius = settings.tp_obstruction_radius;
    obstruction.margin = settings.tp_obstruction_margin;
    scene::CameraDamping& damping = Ensure<scene::CameraDamping>(world, entity);
    damping.position_half_life = settings.tp_position_half_life;
    damping.rotation_half_life = settings.tp_rotation_half_life;
    RemoveIfPresent<scene::CameraLocalOffset>(world, entity);
    RemoveIfPresent<scene::CameraFraming>(world, entity);
  }
}

scene::CameraStackResult ToggleCharacterViewMode(ecs::World& world, ecs::Entity entity,
                                                 ecs::Entity output, ecs::Entity mode,
                                                 const CharacterViewSettings& settings,
                                                 scene::CameraTransitionSpec transition) {
  CharacterViewMode* view = world.Get<CharacterViewMode>(entity);
  if (!view) return scene::CameraStackResult::kInvalidMode;

  view->kind = view->kind == CharacterViewKind::kFirstPerson ? CharacterViewKind::kThirdPerson
                                                             : CharacterViewKind::kFirstPerson;

  // Retire a previous toggle's duplicate stack entry with a silent cut (same
  // view, so invisible) so the stack stays bounded to base + one pushed mode.
  if (view->transition) {
    scene::ReleaseCameraMode(world, view->transition, {.duration = 0});
    view->transition = {};
  }
  ApplyCharacterViewMode(world, entity, settings);

  // Push the reconfigured mode; BeginTransition captures the pre-switch output
  // view as the source and blends toward the new recipe's live view.
  scene::CameraPushResult pushed = scene::PushCameraMode(world, output, mode, transition);
  if (pushed.result == scene::CameraStackResult::kSuccess) view->transition = pushed.activation;
  return pushed.result;
}

bool ApplyCharacterZoom(ecs::World& world, ecs::Entity entity, f32 zoom_delta,
                        bool allow_mode_switch, const CharacterViewSettings& settings) {
  CharacterViewMode* view = world.Get<CharacterViewMode>(entity);
  if (!view) return false;

  if (view->kind == CharacterViewKind::kThirdPerson) {
    scene::CameraBoom* boom = world.Get<scene::CameraBoom>(entity);
    const f32 current = boom ? boom->distance : settings.tp_distance;
    const f32 desired = current - zoom_delta;  // scroll in shortens the boom
    if (allow_mode_switch && desired < settings.tp_min_distance) {
      view->kind = CharacterViewKind::kFirstPerson;
      ApplyCharacterViewMode(world, entity, settings);
      return true;
    }
    if (boom) {
      boom->distance =
          std::clamp(desired, settings.tp_min_distance, settings.tp_max_distance);
    }
    return false;
  }

  // First person: zooming out restores third person at the minimum boom.
  if (allow_mode_switch && zoom_delta < 0) {
    view->kind = CharacterViewKind::kThirdPerson;
    ApplyCharacterViewMode(world, entity, settings);
    if (scene::CameraBoom* boom = world.Get<scene::CameraBoom>(entity)) {
      boom->distance = settings.tp_min_distance;
    }
    return true;
  }
  return false;
}

void TeleportCharacter(ecs::World& world, physics::PhysicsWorld& physics, ecs::Entity entity,
                       const Vec3& feet_position) {
  CharacterBody* body = world.Get<CharacterBody>(entity);
  CharacterState* state = world.Get<CharacterState>(entity);
  if (!body || body->id == 0) return;
  const Vec3 center = feet_position + Vec3{0, body->half_height + body->radius, 0};
  physics.SetCharacterPosition(body->id, center);
  if (state) {
    state->velocity = {0, 0, 0};
    state->time_since_grounded = 0;
    state->teleported = true;
    // Snap the feel smoothers across the discontinuity: no eye lag, no stale dip
    // or buffered jump carries over the cut.
    state->view_initialized = false;
    state->landing_dip = 0;
    state->jump_buffer_timer = 0;
    state->jump_consumed = false;
    state->gait_speed = 0;
  }
  if (scene::Transform* t = world.Get<scene::Transform>(entity)) {
    t->position[0] = feet_position.x;
    t->position[1] = feet_position.y;
    t->position[2] = feet_position.z;
  }
}

}  // namespace rx::character
