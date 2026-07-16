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

        // --- Horizontal velocity ---------------------------------------------
        const f32 gait_speed = stance == CharacterStance::kCrouching
                                   ? settings.crouch_speed
                                   : GaitSpeed(settings, intent.gait);
        const Vec3 move_h{intent.move.x, 0, intent.move.z};
        const f32 throttle = std::clamp(Length(move_h), 0.0f, 1.0f);
        const Vec3 dir = throttle > 1e-4f ? Normalize(move_h) : Vec3{0, 0, 0};
        const Vec3 target_h = dir * (gait_speed * throttle);

        Vec3 horizontal{state.velocity.x, 0, state.velocity.z};
        f32 accel = throttle > 1e-4f ? settings.ground_acceleration : settings.ground_deceleration;
        if (!state.grounded) accel = settings.ground_acceleration * settings.air_control;
        horizontal = MoveTowardVec(horizontal, target_h, std::max(accel, 0.0f) * dt);

        // --- Vertical velocity + jump ----------------------------------------
        f32 vy = state.velocity.y;
        if (state.grounded) {
          if (vy < 0) vy = 0;
          if (intent.jump) vy = std::sqrt(2.0f * std::max(settings.gravity, 0.0f) *
                                          std::max(settings.jump_height, 0.0f));
        }
        vy -= settings.gravity * dt;

        Vec3 velocity{horizontal.x, vy, horizontal.z};

        // --- Drive the controller --------------------------------------------
        Vec3 out_pos = center;
        bool grounded = false;
        physics.MoveCharacterVelocity(body.id, velocity, dt, &out_pos, &grounded, nullptr);

        state.grounded = grounded;
        if (grounded && velocity.y < 0) velocity.y = 0;
        state.velocity = velocity;
        state.time_since_grounded = grounded ? 0.0f : state.time_since_grounded + dt;

        const Vec3 out_feet = out_pos - up * (body.half_height + body.radius);
        transform.position[0] = out_feet.x;
        transform.position[1] = out_feet.y;
        transform.position[2] = out_feet.z;
        const Quat q = HeadingQuat(state.yaw);
        transform.rotation[0] = q.x;
        transform.rotation[1] = q.y;
        transform.rotation[2] = q.z;
        transform.rotation[3] = q.w;

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
        anchor.position = feet + Vec3{0, state.eye_height, 0};
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
  }
  if (scene::Transform* t = world.Get<scene::Transform>(entity)) {
    t->position[0] = feet_position.x;
    t->position[1] = feet_position.y;
    t->position[2] = feet_position.z;
  }
}

}  // namespace rx::character
