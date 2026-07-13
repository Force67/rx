#ifndef RX_SCENE_CAMERA_H_
#define RX_SCENE_CAMERA_H_

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/math.h"
#include "ecs/entity.h"

namespace rx::ecs {
class World;
}

namespace rx::scene {

enum class CameraProjection : u8 { kPerspective, kOrthographic };

struct CameraLens {
  CameraProjection projection = CameraProjection::kPerspective;
  f32 fov_y = 1.0472f;
  f32 ortho_height = 10.0f;
  f32 ortho_near = 0.1f;
  f32 ortho_far = 1000.0f;
};

struct CameraView {
  Vec3 position{0, 0, 3};
  Quat orientation{};
  CameraLens lens;
};

// Common output written by a mode-specific system. A new camera mode is an
// entity with this component plus whatever data its producer needs.
struct CameraMode {
  CameraView view;
  u32 discontinuity_revision = 0;
};

enum class CameraEasing : u8 { kLinear, kSmoothStep, kSmootherStep };

struct CameraTransitionSpec {
  f32 duration = 0.35f;
  CameraEasing easing = CameraEasing::kSmoothStep;
};

struct CameraActivation {
  ecs::Entity output;
  u64 id = 0;

  explicit operator bool() const { return bool(output); }
};

struct CameraStackEntry {
  ecs::Entity mode;
  u64 activation_id = 0;
};

struct CameraTransition {
  CameraView source;
  f32 elapsed = 0;
  f32 duration = 0;
  CameraEasing easing = CameraEasing::kSmoothStep;
  bool active = false;
};

// Dynamic by design: cinematic, UI and gameplay owners can layer modes without
// a fixed engine limit. Rx archetypes move-construct non-trivial components.
struct CameraStack {
  base::Vector<CameraStackEntry> entries;
  CameraTransition transition;
  u64 next_activation_id = 1;
};

struct CameraOutput {
  CameraView view;
  ecs::Entity active_mode;
  u32 observed_discontinuity_revision = 0;
  u64 history_revision = 0;
  bool valid = false;
};

enum class CameraStackResult : u8 {
  kSuccess,
  kInvalidOutput,
  kInvalidMode,
  kInvalidBase,
  kInvalidActivation,
  kNotTop,
  kBaseMode,
};

struct CameraPushResult {
  CameraStackResult result = CameraStackResult::kInvalidOutput;
  CameraActivation activation;
};

// Structural initialization. Call outside World::Each.
RX_SCENE_EXPORT CameraStackResult InitializeCameraStack(ecs::World& world, ecs::Entity output,
                                                        ecs::Entity base_mode);

// Stack mutations only change existing components and are safe from systems
// that are not concurrently iterating CameraStack.
RX_SCENE_EXPORT CameraPushResult PushCameraMode(ecs::World& world, ecs::Entity output,
                                                ecs::Entity mode,
                                                CameraTransitionSpec transition = {});
RX_SCENE_EXPORT CameraStackResult PopCameraMode(ecs::World& world, CameraActivation activation,
                                                CameraTransitionSpec transition = {});
RX_SCENE_EXPORT CameraStackResult ReleaseCameraMode(ecs::World& world, CameraActivation activation,
                                                    CameraTransitionSpec transition = {});
RX_SCENE_EXPORT bool IsCameraModeTop(ecs::World& world, CameraActivation activation);

// Increment after a teleport, replay seek or other discontinuous pose change.
// The active output cuts to the new pose and advances its history revision.
RX_SCENE_EXPORT void InvalidateCameraMode(ecs::World& world, ecs::Entity mode);

RX_SCENE_EXPORT CameraView InterpolateCameraView(const CameraView& source,
                                                 const CameraView& destination, f32 amount);

// Render-rate ECS system. Mode producer systems update CameraMode::view first;
// this resolves every CameraStack + CameraOutput entity afterward.
RX_SCENE_EXPORT void ResolveCameraStacks(ecs::World& world, f32 dt);

inline Vec3 CameraForward(const CameraView& view) {
  return Rotate(view.orientation, Vec3{0, 0, -1});
}

inline Vec3 CameraUp(const CameraView& view) { return Rotate(view.orientation, Vec3{0, 1, 0}); }

}  // namespace rx::scene

#endif  // RX_SCENE_CAMERA_H_
