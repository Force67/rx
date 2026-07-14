#ifndef RX_SCENE_CAMERA_RIG_H_
#define RX_SCENE_CAMERA_RIG_H_

#include "scene/camera.h"

namespace rx::scene {

// World-space target data supplied by the game. Increment revision when the
// anchor teleports or otherwise changes discontinuously.
struct CameraAnchor {
  Vec3 position;
  Quat orientation;
  Vec3 velocity;
  u32 revision = 0;
};

// Frame intent supplied by input, AI, replay or tooling. Angular deltas and
// recenter are consumed by BuildCameraRigs; aim_direction remains available to
// game-specific framing systems.
struct CameraIntent {
  f32 yaw_delta = 0;
  f32 pitch_delta = 0;
  f32 zoom_delta = 0;
  Vec3 aim_direction{0, 0, -1};
  bool recenter = false;
};

// Intermediate and persistent state shared by the composable rig stages.
struct CameraRigPose {
  CameraView desired;
  CameraView candidate;
  CameraView resolved;
  Vec3 pivot;
  u32 observed_anchor_revision = 0;
  bool initialized = false;
  bool snap = false;
  bool prepared = false;
};

enum class CameraOrbitSpace : u8 { kWorld, kAnchor };

struct CameraOrbit {
  f32 yaw = 0;
  f32 pitch = 0;
  f32 min_pitch = -1.4f;
  f32 max_pitch = 1.4f;
  CameraOrbitSpace space = CameraOrbitSpace::kWorld;
};

enum class CameraOffsetSpace : u8 { kWorld, kAnchor, kView };

struct CameraLocalOffset {
  Vec3 offset;
  CameraOffsetSpace space = CameraOffsetSpace::kAnchor;
};

struct CameraBoom {
  f32 distance = 3.0f;
  f32 shoulder_offset = 0;
  f32 height_offset = 0;
};

struct CameraRecenter {
  f32 delay = 0.75f;
  f32 half_life = 0.25f;
  f32 minimum_speed = 0.1f;
  f32 idle_time = 0;
};

struct CameraLookAhead {
  f32 seconds = 0.2f;
  f32 maximum_distance = 2.0f;
};

// A world-axis follow region. The anchor can move inside half_extent without
// moving the tracked center, useful for platformers, strategy and framing rigs.
struct CameraFollowDeadZone {
  Vec3 half_extent;
  Vec3 center;
  bool initialized = false;
};

enum CameraAxisMask : u8 {
  kCameraAxisNone = 0,
  kCameraAxisX = 1 << 0,
  kCameraAxisY = 1 << 1,
  kCameraAxisZ = 1 << 2,
};

struct CameraAxisLock {
  Vec3 value;
  u8 axes = kCameraAxisNone;
};

// Rotates the camera toward pivot + target_offset after offsets and boom are
// applied. Weight 0 preserves orbit orientation; 1 fully frames the target.
struct CameraFraming {
  Vec3 target_offset;
  CameraOffsetSpace target_space = CameraOffsetSpace::kWorld;
  Vec3 up{0, 1, 0};
  CameraOffsetSpace up_space = CameraOffsetSpace::kAnchor;
  f32 weight = 1.0f;
};

struct CameraDamping {
  f32 position_half_life = 0;
  f32 rotation_half_life = 0;
  f32 lens_half_life = 0;
};

struct CameraLensDrive {
  CameraLens lens;
  f32 zoom_speed = 0;
  f32 minimum_fov = 0.1745f;
  f32 maximum_fov = 2.9671f;
  f32 minimum_ortho_height = 0.01f;
  f32 maximum_ortho_height = 10000.0f;
};

enum class CameraMissingObstructionResult : u8 {
  kHoldLastPose,
  kUseCandidate,
};

// Physics-neutral synchronous request/result. PrepareCameraRigConstraints
// writes a new request ID. The game answers that current request before resolve;
// late IDs are rejected. A rig with slower/asynchronous physics can safely hold
// its last pose between answers or explicitly opt into the untested candidate.
struct CameraObstruction {
  f32 radius = 0.2f;
  f32 margin = 0.1f;
  u64 mask = ~u64{0};

  Vec3 origin;
  Vec3 desired_position;
  Vec3 safe_position;
  u64 request_id = 0;
  u64 result_request_id = 0;
  bool obstructed = false;
  bool has_result = false;
  CameraMissingObstructionResult missing_result = CameraMissingObstructionResult::kHoldLastPose;
};

inline bool SetCameraObstructionResult(CameraObstruction& obstruction, u64 request_id,
                                       const Vec3& safe_position, bool obstructed) {
  if (request_id != obstruction.request_id) return false;
  obstruction.safe_position = safe_position;
  obstruction.obstructed = obstructed;
  obstruction.result_request_id = request_id;
  obstruction.has_result = true;
  return true;
}

// Stage 1: consume anchor/intent and compose desired poses. Games may run custom
// desired-pose systems after this returns.
RX_SCENE_EXPORT void BuildCameraRigs(ecs::World& world, f32 dt);

// Stage 2: apply damping to a candidate pose and emit CameraObstruction
// requests. The game answers those requests before resolve.
RX_SCENE_EXPORT void PrepareCameraRigConstraints(ecs::World& world, f32 dt);

// Stage 3: apply obstruction results and framing, then write CameraMode::view.
// Run ResolveCameraStacks after this function.
RX_SCENE_EXPORT void ResolveCameraRigs(ecs::World& world, f32 dt);

}  // namespace rx::scene

#endif  // RX_SCENE_CAMERA_RIG_H_
