#include "scene/camera_rig.h"

#include <algorithm>
#include <cmath>

#include "ecs/world.h"

namespace rx::scene {
namespace {

constexpr f32 kPi = 3.14159265358979323846f;
constexpr f32 kTwoPi = 2.0f * kPi;

f32 BlendForHalfLife(f32 half_life, f32 dt) {
  if (half_life <= 0) return 1.0f;
  return 1.0f - std::exp2(-dt / half_life);
}

f32 MoveAngle(f32 current, f32 target, f32 amount) {
  return current + std::remainder(target - current, kTwoPi) * amount;
}

f32 YawFromDirection(const Vec3& direction) { return std::atan2(direction.x, -direction.z); }

Quat OrbitRotation(f32 yaw, f32 pitch) {
  return Normalize(QuatFromAxisAngle({0, -1, 0}, yaw) * QuatFromAxisAngle({1, 0, 0}, pitch));
}

Quat LookRotation(const Vec3& direction, const Vec3& up_reference, const Quat& fallback) {
  const Vec3 forward = Normalize(direction);
  if (Length(forward) <= 1e-5f) return fallback;

  Vec3 right = Cross(forward, Normalize(up_reference));
  if (Length(right) <= 1e-5f) right = Cross(forward, Rotate(fallback, Vec3{0, 1, 0}));
  if (Length(right) <= 1e-5f) {
    const Vec3 axis =
        std::abs(forward.x) <= std::abs(forward.y) && std::abs(forward.x) <= std::abs(forward.z)
            ? Vec3{1, 0, 0}
            : (std::abs(forward.y) <= std::abs(forward.z) ? Vec3{0, 1, 0} : Vec3{0, 0, 1});
    right = Cross(forward, axis);
  }
  right = Normalize(right);
  const Vec3 up = Normalize(Cross(right, forward));
  const Vec3 back = forward * -1.0f;

  Mat4 rotation = Mat4::Identity();
  rotation.m[0] = right.x;
  rotation.m[1] = right.y;
  rotation.m[2] = right.z;
  rotation.m[4] = up.x;
  rotation.m[5] = up.y;
  rotation.m[6] = up.z;
  rotation.m[8] = back.x;
  rotation.m[9] = back.y;
  rotation.m[10] = back.z;
  return Normalize(QuatFromMat4(rotation));
}

Vec3 ResolveOffset(const Vec3& offset, CameraOffsetSpace space, const CameraAnchor& anchor,
                   const CameraRigPose& pose) {
  switch (space) {
    case CameraOffsetSpace::kWorld:
      return offset;
    case CameraOffsetSpace::kAnchor:
      return Rotate(Normalize(anchor.orientation), offset);
    case CameraOffsetSpace::kView:
      return Rotate(pose.desired.orientation, offset);
  }
  return offset;
}

Vec3 ClampLength(const Vec3& value, f32 maximum) {
  if (maximum <= 0) return {};
  const f32 length = Length(value);
  if (length <= maximum) return value;
  return value * (maximum / length);
}

void UpdateDeadZone(CameraFollowDeadZone& dead_zone, const Vec3& anchor) {
  if (!dead_zone.initialized) {
    dead_zone.center = anchor;
    dead_zone.initialized = true;
    return;
  }

  auto update_axis = [](f32 value, f32 extent, f32* center) {
    extent = std::max(extent, 0.0f);
    if (value < *center - extent) *center = value + extent;
    if (value > *center + extent) *center = value - extent;
  };
  update_axis(anchor.x, dead_zone.half_extent.x, &dead_zone.center.x);
  update_axis(anchor.y, dead_zone.half_extent.y, &dead_zone.center.y);
  update_axis(anchor.z, dead_zone.half_extent.z, &dead_zone.center.z);
}

CameraLens DampLens(const CameraLens& current, const CameraLens& target, f32 amount) {
  if (current.projection != target.projection) return target;

  CameraLens lens = target;
  if (target.projection == CameraProjection::kPerspective) {
    const f32 current_scale = std::tan(current.fov_y * 0.5f);
    const f32 target_scale = std::tan(target.fov_y * 0.5f);
    lens.fov_y = 2.0f * std::atan(std::lerp(current_scale, target_scale, amount));
  } else {
    lens.ortho_height = std::lerp(current.ortho_height, target.ortho_height, amount);
    lens.ortho_near = std::lerp(current.ortho_near, target.ortho_near, amount);
    lens.ortho_far = std::lerp(current.ortho_far, target.ortho_far, amount);
  }
  return lens;
}

}  // namespace

void BuildCameraRigs(ecs::World& world, f32 dt) {
  if (!std::isfinite(dt) || dt < 0) dt = 0;

  world.Each<CameraRigPose, CameraAnchor, CameraMode>([&](ecs::Entity entity, CameraRigPose& pose,
                                                          CameraAnchor& anchor, CameraMode& mode) {
    const CameraLens lens = pose.initialized ? pose.desired.lens : mode.view.lens;
    pose.desired = {
        .position = anchor.position, .orientation = Normalize(anchor.orientation), .lens = lens};
    pose.pivot = anchor.position;
    pose.prepared = false;

    if (!pose.initialized) {
      pose.observed_anchor_revision = anchor.revision;
    } else if (pose.observed_anchor_revision != anchor.revision) {
      pose.observed_anchor_revision = anchor.revision;
      pose.snap = true;
    }

    if (CameraFollowDeadZone* dead_zone = world.Get<CameraFollowDeadZone>(entity)) {
      if (pose.snap) dead_zone->initialized = false;
      UpdateDeadZone(*dead_zone, anchor.position);
      pose.pivot = dead_zone->center;
      pose.desired.position = pose.pivot;
    }

    if (CameraLookAhead* look_ahead = world.Get<CameraLookAhead>(entity)) {
      const Vec3 offset = ClampLength(anchor.velocity * std::max(look_ahead->seconds, 0.0f),
                                      std::max(look_ahead->maximum_distance, 0.0f));
      pose.pivot += offset;
      pose.desired.position = pose.pivot;
    }

    if (CameraAxisLock* lock = world.Get<CameraAxisLock>(entity)) {
      if (lock->axes & kCameraAxisX) pose.pivot.x = lock->value.x;
      if (lock->axes & kCameraAxisY) pose.pivot.y = lock->value.y;
      if (lock->axes & kCameraAxisZ) pose.pivot.z = lock->value.z;
      pose.desired.position = pose.pivot;
    }
  });

  world.Each<CameraRigPose, CameraAnchor, CameraOrbit, CameraMode>(
      [&](ecs::Entity entity, CameraRigPose& pose, CameraAnchor& anchor, CameraOrbit& orbit,
          CameraMode&) {
        CameraIntent* intent = world.Get<CameraIntent>(entity);
        const f32 yaw_delta = intent ? intent->yaw_delta : 0;
        const f32 pitch_delta = intent ? intent->pitch_delta : 0;
        orbit.yaw += yaw_delta;
        orbit.pitch =
            std::clamp(orbit.pitch + pitch_delta, std::min(orbit.min_pitch, orbit.max_pitch),
                       std::max(orbit.min_pitch, orbit.max_pitch));
        const Quat anchor_rotation = Normalize(anchor.orientation);

        if (CameraRecenter* recenter = world.Get<CameraRecenter>(entity)) {
          const bool manual = std::abs(yaw_delta) > 1e-6f || std::abs(pitch_delta) > 1e-6f;
          if (manual) {
            recenter->idle_time = 0;
          } else {
            recenter->idle_time += dt;
          }

          const Vec3 recenter_velocity = orbit.space == CameraOrbitSpace::kAnchor
                                             ? Rotate(Conjugate(anchor_rotation), anchor.velocity)
                                             : anchor.velocity;
          const Vec3 horizontal_velocity{recenter_velocity.x, 0, recenter_velocity.z};
          const bool requested = intent && intent->recenter;
          const bool moving = Length(horizontal_velocity) >
                              std::max(std::max(recenter->minimum_speed, 0.0f), 1e-5f);
          if (requested || (moving && recenter->idle_time >= std::max(recenter->delay, 0.0f))) {
            const f32 target_yaw =
                moving ? YawFromDirection(horizontal_velocity)
                       : (orbit.space == CameraOrbitSpace::kAnchor
                              ? 0.0f
                              : YawFromDirection(Rotate(anchor_rotation, Vec3{0, 0, -1})));
            orbit.yaw = MoveAngle(orbit.yaw, target_yaw, BlendForHalfLife(recenter->half_life, dt));
          }
        }

        const Quat local = OrbitRotation(orbit.yaw, orbit.pitch);
        pose.desired.orientation =
            orbit.space == CameraOrbitSpace::kAnchor ? Normalize(anchor_rotation * local) : local;

        if (intent) {
          intent->yaw_delta = 0;
          intent->pitch_delta = 0;
          intent->recenter = false;
        }
      });

  world.Each<CameraRigPose, CameraAnchor, CameraLocalOffset, CameraMode>(
      [](ecs::Entity, CameraRigPose& pose, CameraAnchor& anchor, CameraLocalOffset& local_offset,
         CameraMode&) {
        const Vec3 offset = ResolveOffset(local_offset.offset, local_offset.space, anchor, pose);
        pose.pivot += offset;
        pose.desired.position = pose.pivot;
      });

  world.Each<CameraRigPose, CameraBoom, CameraMode>(
      [](ecs::Entity, CameraRigPose& pose, CameraBoom& boom, CameraMode&) {
        const Vec3 forward = Rotate(pose.desired.orientation, {0, 0, -1});
        const Vec3 right = Rotate(pose.desired.orientation, {1, 0, 0});
        const Vec3 up = Rotate(pose.desired.orientation, {0, 1, 0});
        pose.desired.position = pose.pivot + forward * -std::max(boom.distance, 0.0f) +
                                right * boom.shoulder_offset + up * boom.height_offset;
      });

  world.Each<CameraRigPose, CameraLensDrive, CameraMode>(
      [&](ecs::Entity entity, CameraRigPose& pose, CameraLensDrive& drive, CameraMode&) {
        if (CameraIntent* intent = world.Get<CameraIntent>(entity)) {
          if (drive.lens.projection == CameraProjection::kPerspective) {
            drive.lens.fov_y = std::clamp(drive.lens.fov_y - intent->zoom_delta * drive.zoom_speed,
                                          std::min(drive.minimum_fov, drive.maximum_fov),
                                          std::max(drive.minimum_fov, drive.maximum_fov));
          } else {
            drive.lens.ortho_height =
                std::clamp(drive.lens.ortho_height - intent->zoom_delta * drive.zoom_speed,
                           std::min(drive.minimum_ortho_height, drive.maximum_ortho_height),
                           std::max(drive.minimum_ortho_height, drive.maximum_ortho_height));
          }
          intent->zoom_delta = 0;
        }
        pose.desired.lens = drive.lens;
      });
}

void PrepareCameraRigConstraints(ecs::World& world, f32 dt) {
  if (!std::isfinite(dt) || dt < 0) dt = 0;

  world.Each<CameraRigPose, CameraAnchor, CameraMode>(
      [&](ecs::Entity entity, CameraRigPose& pose, CameraAnchor&, CameraMode&) {
        pose.candidate = pose.desired;
        if (pose.initialized && !pose.snap) {
          if (CameraDamping* damping = world.Get<CameraDamping>(entity)) {
            pose.candidate.position = Lerp(pose.resolved.position, pose.desired.position,
                                           BlendForHalfLife(damping->position_half_life, dt));
            pose.candidate.lens = DampLens(pose.resolved.lens, pose.desired.lens,
                                           BlendForHalfLife(damping->lens_half_life, dt));
          }
        }

        if (CameraObstruction* obstruction = world.Get<CameraObstruction>(entity)) {
          ++obstruction->request_id;
          if (obstruction->request_id == 0) ++obstruction->request_id;
          obstruction->origin = pose.pivot;
          obstruction->desired_position = pose.candidate.position;
          obstruction->safe_position = pose.candidate.position;
          obstruction->obstructed = false;
          obstruction->has_result = false;
        }
        pose.prepared = true;
      });
}

void ResolveCameraRigs(ecs::World& world, f32 dt) {
  if (!std::isfinite(dt) || dt < 0) dt = 0;

  world.Each<CameraRigPose, CameraAnchor, CameraMode>([&](ecs::Entity entity, CameraRigPose& pose,
                                                          CameraAnchor& anchor, CameraMode& mode) {
    if (!pose.prepared) return;

    const bool anchor_cut = pose.initialized && pose.snap;
    CameraView next = pose.candidate;
    bool hold_pose = false;

    if (CameraObstruction* obstruction = world.Get<CameraObstruction>(entity)) {
      const bool current_result =
          obstruction->has_result && obstruction->result_request_id == obstruction->request_id;
      if (current_result) {
        if (obstruction->obstructed) next.position = obstruction->safe_position;
      } else if (obstruction->missing_result == CameraMissingObstructionResult::kHoldLastPose &&
                 pose.initialized && !pose.snap) {
        next = pose.resolved;
        hold_pose = true;
      }
    }

    Quat target_orientation = pose.desired.orientation;
    if (!hold_pose) {
      if (CameraFraming* framing = world.Get<CameraFraming>(entity)) {
        const Vec3 target =
            pose.pivot + ResolveOffset(framing->target_offset, framing->target_space, anchor, pose);
        const Vec3 up = ResolveOffset(framing->up, framing->up_space, anchor, pose);
        const Vec3 to_target = target - next.position;
        if (Length(to_target) > 1e-5f) {
          target_orientation =
              Slerp(target_orientation, LookRotation(to_target, up, target_orientation),
                    std::clamp(framing->weight, 0.0f, 1.0f));
        }
      }

      next.orientation = target_orientation;
      if (pose.initialized && !pose.snap) {
        if (CameraDamping* damping = world.Get<CameraDamping>(entity)) {
          next.orientation = Slerp(pose.resolved.orientation, target_orientation,
                                   BlendForHalfLife(damping->rotation_half_life, dt));
        }
      }
    }

    next.orientation = Normalize(next.orientation);
    const bool projection_cut =
        pose.initialized &&
        pose.resolved.lens.projection != next.lens.projection;
    pose.resolved = next;
    pose.initialized = true;
    pose.snap = false;
    pose.prepared = false;
    mode.view = next;
    if (anchor_cut || projection_cut) ++mode.discontinuity_revision;
  });
}

}  // namespace rx::scene
