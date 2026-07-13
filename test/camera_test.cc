#include "scene/camera.h"

#include <base/containers/vector.h>

#include <cmath>
#include <cstdio>

#include "ecs/world.h"

namespace {

using base::Vector;
using rx::f32;
using namespace rx::ecs;
using namespace rx::scene;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "camera_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-4f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "camera_test: FAIL: %s (got %.6f, expected %.6f)\n", message, actual,
               expected);
  ++failures;
}

Entity MakeMode(World& world, f32 x, CameraProjection projection = CameraProjection::kPerspective) {
  Entity entity = world.Create();
  CameraMode mode;
  mode.view.position = {x, 0, 0};
  mode.view.lens.projection = projection;
  world.Add(entity, mode);
  return entity;
}

Entity MakeOutput(World& world, Entity base_mode) {
  Entity output = world.Create();
  Check(InitializeCameraStack(world, output, base_mode) == CameraStackResult::kSuccess,
        "initialize camera stack");
  return output;
}

void TestDynamicStackSurvivesArchetypeGrowth() {
  World world;
  Entity base_mode = MakeMode(world, 0);
  Vector<Entity> outputs;
  for (int i = 0; i < 128; ++i) outputs.push_back(MakeOutput(world, base_mode));

  CameraStack* first_stack = world.Get<CameraStack>(outputs[0]);
  Check(first_stack && first_stack->entries.size() == 1,
        "vector-backed component survives column relocation");
  Check(first_stack && first_stack->entries[0].mode == base_mode,
        "relocated stack retains its base mode");

  Entity overlay = MakeMode(world, 8);
  CameraPushResult pushed = PushCameraMode(world, outputs[0], overlay, {.duration = 0});
  Check(pushed.result == CameraStackResult::kSuccess, "push after archetype relocation");
  Check(world.Get<CameraStack>(outputs[0])->entries.size() == 2,
        "dynamic stack grows after archetype relocation");
}

void TestAnimatedTransitionUsesLiveDestination() {
  World world;
  Entity base_mode = MakeMode(world, 0);
  Entity output = MakeOutput(world, base_mode);
  Entity overlay = MakeMode(world, 10);

  CameraPushResult pushed =
      PushCameraMode(world, output, overlay, {.duration = 1.0f, .easing = CameraEasing::kLinear});
  Check(pushed.result == CameraStackResult::kSuccess, "push animated mode");

  ResolveCameraStacks(world, 0.25f);
  Near(world.Get<CameraOutput>(output)->view.position.x, 2.5f, "transition reaches quarter pose");

  world.Get<CameraMode>(overlay)->view.position.x = 14.0f;
  ResolveCameraStacks(world, 0.25f);
  Near(world.Get<CameraOutput>(output)->view.position.x, 7.0f,
       "transition tracks a moving destination");

  ResolveCameraStacks(world, 0.5f);
  Near(world.Get<CameraOutput>(output)->view.position.x, 14.0f,
       "transition reaches live destination");
  Check(!world.Get<CameraStack>(output)->transition.active, "completed transition stops");
}

void TestInterruptedTransitionStartsAtResolvedPose() {
  World world;
  Entity base_mode = MakeMode(world, 0);
  Entity output = MakeOutput(world, base_mode);
  Entity first = MakeMode(world, 10);
  Entity second = MakeMode(world, 20);

  PushCameraMode(world, output, first, {.duration = 1.0f, .easing = CameraEasing::kLinear});
  ResolveCameraStacks(world, 0.5f);
  Near(world.Get<CameraOutput>(output)->view.position.x, 5.0f, "first transition midpoint");

  PushCameraMode(world, output, second, {.duration = 1.0f, .easing = CameraEasing::kLinear});
  ResolveCameraStacks(world, 0.0f);
  Near(world.Get<CameraOutput>(output)->view.position.x, 5.0f,
       "interruption is position-continuous");
  ResolveCameraStacks(world, 0.5f);
  Near(world.Get<CameraOutput>(output)->view.position.x, 12.5f,
       "interrupted transition animates from resolved pose");
}

void TestActivationOwnership() {
  World world;
  Entity base_mode = MakeMode(world, 0);
  Entity output = MakeOutput(world, base_mode);
  CameraPushResult first = PushCameraMode(world, output, MakeMode(world, 10), {.duration = 0});
  CameraPushResult second = PushCameraMode(world, output, MakeMode(world, 20), {.duration = 0});

  Check(PopCameraMode(world, first.activation, {.duration = 0}) == CameraStackResult::kNotTop,
        "pop rejects an obscured activation");
  Check(ReleaseCameraMode(world, first.activation, {.duration = 0}) == CameraStackResult::kSuccess,
        "release removes an obscured activation");
  Check(PopCameraMode(world, second.activation, {.duration = 0}) == CameraStackResult::kSuccess,
        "pop removes the current activation");
  Near(world.Get<CameraOutput>(output)->view.position.x, 0.0f,
       "removed obscured mode does not resurface");
  Check(PopCameraMode(world, {output, 0}, {.duration = 0}) == CameraStackResult::kBaseMode,
        "base mode cannot be popped");
}

void TestCutsAndStaleModes() {
  World world;
  Entity base_mode = MakeMode(world, 0);
  Entity output = MakeOutput(world, base_mode);
  Entity overlay = MakeMode(world, 10);
  CameraPushResult pushed =
      PushCameraMode(world, output, overlay, {.duration = 1.0f, .easing = CameraEasing::kLinear});
  ResolveCameraStacks(world, 0.5f);

  const rx::u64 before_stale = world.Get<CameraOutput>(output)->history_revision;
  world.Destroy(overlay);
  ResolveCameraStacks(world, 0);
  Near(world.Get<CameraOutput>(output)->view.position.x, 0.0f,
       "destroyed top mode falls back to base");
  Check(world.Get<CameraOutput>(output)->history_revision == before_stale + 1,
        "stale top advances camera history");
  Check(!IsCameraModeTop(world, pushed.activation), "destroyed activation is no longer top");

  world.Get<CameraMode>(base_mode)->view.position.x = 30.0f;
  const rx::u64 before_teleport = world.Get<CameraOutput>(output)->history_revision;
  InvalidateCameraMode(world, base_mode);
  ResolveCameraStacks(world, 0);
  Near(world.Get<CameraOutput>(output)->view.position.x, 30.0f,
       "invalidated active mode cuts to new pose");
  Check(world.Get<CameraOutput>(output)->history_revision == before_teleport + 1,
        "explicit invalidation advances camera history");
}

void TestLensAndOrientationInterpolation() {
  CameraView source;
  source.lens.fov_y = 1.0471975512f;
  CameraView destination;
  destination.orientation = rx::QuatFromAxisAngle({0, 1, 0}, 3.1415926535f);
  destination.lens.fov_y = 1.5707963268f;

  CameraView half = InterpolateCameraView(source, destination, 0.5f);
  const f32 expected_fov =
      2.0f * std::atan(std::lerp(std::tan(source.lens.fov_y * 0.5f),
                                 std::tan(destination.lens.fov_y * 0.5f), 0.5f));
  Near(half.lens.fov_y, expected_fov, "field of view blends in focal scale");
  Near(rx::Length(CameraForward(half)), 1.0f, "slerped orientation remains normalized");

  World world;
  Entity base_mode = MakeMode(world, 0, CameraProjection::kPerspective);
  Entity output = MakeOutput(world, base_mode);
  Entity ortho = MakeMode(world, 5, CameraProjection::kOrthographic);
  const rx::u64 before = world.Get<CameraOutput>(output)->history_revision;
  PushCameraMode(world, output, ortho, {.duration = 2.0f});
  Check(!world.Get<CameraStack>(output)->transition.active,
        "projection type change does not animate");
  Check(world.Get<CameraOutput>(output)->history_revision == before + 1,
        "projection type change advances history");
}

}  // namespace

int main() {
  TestDynamicStackSurvivesArchetypeGrowth();
  TestAnimatedTransitionUsesLiveDestination();
  TestInterruptedTransitionStartsAtResolvedPose();
  TestActivationOwnership();
  TestCutsAndStaleModes();
  TestLensAndOrientationInterpolation();

  if (failures != 0) {
    std::fprintf(stderr, "camera_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("camera_test: PASS\n");
  return 0;
}
