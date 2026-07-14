#include "scene/camera_rig.h"

#include <cmath>
#include <cstdio>

#include "ecs/world.h"

namespace {

using rx::f32;
using rx::Length;
using rx::u64;
using rx::Vec3;
using namespace rx::ecs;
using namespace rx::scene;

constexpr f32 kPi = 3.14159265358979323846f;
int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "camera_rig_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-4f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "camera_rig_test: FAIL: %s (got %.6f, expected %.6f)\n", message, actual,
               expected);
  ++failures;
}

void NearVec(const Vec3& actual, const Vec3& expected, const char* message, f32 epsilon = 1e-4f) {
  if (Length(actual - expected) <= epsilon) return;
  std::fprintf(stderr,
               "camera_rig_test: FAIL: %s (got %.4f, %.4f, %.4f; expected "
               "%.4f, %.4f, %.4f)\n",
               message, actual.x, actual.y, actual.z, expected.x, expected.y, expected.z);
  ++failures;
}

Entity MakeRig(World& world, const CameraAnchor& anchor = {}) {
  Entity entity = world.Create();
  world.Add(entity, anchor);
  world.Add(entity, CameraRigPose{});
  world.Add(entity, CameraMode{});
  return entity;
}

void StepRigs(World& world, f32 dt) {
  BuildCameraRigs(world, dt);
  PrepareCameraRigConstraints(world, dt);
  ResolveCameraRigs(world, dt);
}

void TestFirstPersonRecipe() {
  World world;
  Entity rig = MakeRig(world, {.position = {10, 2, 3}});
  world.Add(rig, CameraIntent{});
  world.Add(rig, CameraOrbit{});
  world.Add(rig, CameraLocalOffset{.offset = {0, 1.7f, 0}});

  StepRigs(world, 1.0f / 60.0f);
  CameraMode* mode = world.Get<CameraMode>(rig);
  NearVec(mode->view.position, {10, 3.7f, 3}, "first-person eye uses an anchor-local offset");
  NearVec(CameraForward(mode->view), {0, 0, -1},
          "first-person default orientation faces camera forward");

  world.Get<CameraIntent>(rig)->yaw_delta = kPi * 0.5f;
  StepRigs(world, 1.0f / 60.0f);
  NearVec(CameraForward(mode->view), {1, 0, 0}, "first-person orbit consumes immediate yaw input");
  Near(world.Get<CameraIntent>(rig)->yaw_delta, 0, "angular input is consumed once");
}

void TestThirdPersonRecipeAndObstruction() {
  World world;
  Entity rig = MakeRig(world);
  world.Add(rig, CameraOrbit{});
  world.Add(rig, CameraLocalOffset{.offset = {0, 1.5f, 0}});
  world.Add(rig, CameraBoom{.distance = 3.0f, .shoulder_offset = 0.5f});
  world.Add(rig, CameraFraming{});
  world.Add(rig, CameraDamping{.position_half_life = 1.0f});
  world.Add(rig, CameraLensDrive{});
  world.Add(rig, CameraObstruction{});

  BuildCameraRigs(world, 0);
  PrepareCameraRigConstraints(world, 0);
  CameraObstruction* obstruction = world.Get<CameraObstruction>(rig);
  NearVec(obstruction->origin, {0, 1.5f, 0}, "third-person obstruction starts at the pivot");
  NearVec(obstruction->desired_position, {0.5f, 1.5f, 3.0f},
          "third-person boom produces shoulder position");

  Check(SetCameraObstructionResult(*obstruction, obstruction->request_id, {0, 1.5f, 1.0f}, true),
        "current obstruction request accepts its result");
  ResolveCameraRigs(world, 0);
  CameraMode* mode = world.Get<CameraMode>(rig);
  NearVec(mode->view.position, obstruction->safe_position, "obstruction retracts immediately");
  const CameraView held = mode->view;

  const u64 stale_request = obstruction->request_id;
  world.Get<CameraOrbit>(rig)->yaw = 0.5f;
  world.Get<CameraLensDrive>(rig)->lens.fov_y = 0.8f;
  BuildCameraRigs(world, 1.0f);
  PrepareCameraRigConstraints(world, 1.0f);
  Check(!obstruction->has_result, "next build clears the previous obstruction result");
  NearVec(obstruction->desired_position, world.Get<CameraRigPose>(rig)->candidate.position,
          "collision probes the damped candidate that would render");
  Check(!SetCameraObstructionResult(*obstruction, stale_request, {0, 0, 0}, true),
        "late obstruction result cannot overwrite a newer request");
  ResolveCameraRigs(world, 1.0f);
  Near(mode->view.position.z, 1.0f,
       "missing physics result conservatively holds an obstructed camera");
  NearVec(CameraForward(mode->view), CameraForward(held),
          "missing physics result holds camera orientation");
  Near(mode->view.lens.fov_y, held.lens.fov_y, "missing physics result holds camera lens");

  BuildCameraRigs(world, 1.0f);
  PrepareCameraRigConstraints(world, 1.0f);
  const Vec3 expected_extension = world.Get<CameraRigPose>(rig)->candidate.position;
  Check(SetCameraObstructionResult(*obstruction, obstruction->request_id,
                                   obstruction->desired_position, false),
        "current clear result is accepted");
  ResolveCameraRigs(world, 1.0f);
  NearVec(mode->view.position, expected_extension,
          "camera extends from an obstruction through damping");

  const Vec3 to_pivot = rx::Normalize(world.Get<CameraRigPose>(rig)->pivot - mode->view.position);
  Check(rx::Dot(CameraForward(mode->view), to_pivot) > 0.99f,
        "third-person framing looks back toward its pivot");
}

void TestFrameRateIndependentDampingAndCuts() {
  World world;
  Entity one_step = MakeRig(world);
  Entity two_steps = MakeRig(world);
  world.Add(one_step, CameraDamping{.position_half_life = 1.0f});
  world.Add(two_steps, CameraDamping{.position_half_life = 1.0f});
  StepRigs(world, 0);

  world.Get<CameraAnchor>(one_step)->position.x = 10;
  world.Get<CameraAnchor>(two_steps)->position.x = 10;
  StepRigs(world, 0.5f);
  BuildCameraRigs(world, 0.5f);
  PrepareCameraRigConstraints(world, 0.5f);
  ResolveCameraRigs(world, 0.5f);

  // one_step also received two half steps above, so compare it to the analytic
  // result and use a separate direct rig for one full half-life.
  Near(world.Get<CameraMode>(one_step)->view.position.x, 5.0f,
       "two half steps move halfway over one half-life");

  World direct_world;
  Entity direct = MakeRig(direct_world);
  direct_world.Add(direct, CameraDamping{.position_half_life = 1.0f});
  StepRigs(direct_world, 0);
  direct_world.Get<CameraAnchor>(direct)->position.x = 10;
  StepRigs(direct_world, 1.0f);
  Near(direct_world.Get<CameraMode>(direct)->view.position.x, 5.0f,
       "one full step matches two half steps");

  CameraMode* mode = direct_world.Get<CameraMode>(direct);
  const u32 revision = mode->discontinuity_revision;
  CameraAnchor* anchor = direct_world.Get<CameraAnchor>(direct);
  anchor->position.x = 30;
  ++anchor->revision;
  StepRigs(direct_world, 0.01f);
  Near(mode->view.position.x, 30, "anchor revision snaps instead of damping a teleport");
  Check(mode->discontinuity_revision == revision + 1,
        "anchor revision propagates a camera discontinuity");
}

void TestStrategyAndPlatformerRecipe() {
  World world;
  Entity rig = MakeRig(world, {.position = {4, 5, 6}, .velocity = {10, 0, 0}});
  world.Add(rig, CameraIntent{.zoom_delta = 3});
  world.Add(rig, CameraFollowDeadZone{.half_extent = {2, 0, 2}});
  world.Add(rig, CameraLookAhead{.seconds = 0.1f, .maximum_distance = 0.5f});
  world.Add(rig, CameraAxisLock{.value = {0, 10, 0}, .axes = kCameraAxisY});

  CameraLensDrive lens;
  lens.lens.projection = CameraProjection::kOrthographic;
  lens.lens.ortho_height = 20;
  lens.zoom_speed = 2;
  world.Add(rig, lens);

  StepRigs(world, 0);
  CameraMode* mode = world.Get<CameraMode>(rig);
  NearVec(mode->view.position, {4.5f, 10, 6}, "dead zone, look-ahead and axis lock compose");
  Near(mode->view.lens.ortho_height, 14, "orthographic zoom is driven from generic intent");

  CameraAnchor* anchor = world.Get<CameraAnchor>(rig);
  anchor->position.x = 5;
  StepRigs(world, 0);
  Near(mode->view.position.x, 4.5f,
       "anchor motion inside the follow dead zone does not move the camera");

  anchor->position.x = 8;
  StepRigs(world, 0);
  Near(mode->view.position.x, 6.5f, "camera follows once the anchor leaves the dead zone");
}

void TestRecenterUsesShortestAngle() {
  World world;
  Entity rig = MakeRig(world, {.velocity = {0, 0, -1}});
  world.Add(rig, CameraIntent{.recenter = true});
  world.Add(rig, CameraOrbit{.yaw = 6.2f});
  world.Add(rig, CameraRecenter{.delay = 0, .half_life = 1.0f, .minimum_speed = 0});

  BuildCameraRigs(world, 1.0f);
  Check(world.Get<CameraOrbit>(rig)->yaw > 6.2f,
        "recenter crosses the angle wrap by the short path");

  World stationary_world;
  Entity stationary = MakeRig(stationary_world);
  stationary_world.Add(stationary, CameraOrbit{.yaw = 1.0f});
  stationary_world.Add(stationary,
                       CameraRecenter{.delay = 0, .half_life = 1.0f, .minimum_speed = 0});
  BuildCameraRigs(stationary_world, 1.0f);
  Near(stationary_world.Get<CameraOrbit>(stationary)->yaw, 1.0f,
       "stationary anchor does not trigger automatic recenter");
}

void TestAnchorSpaceRecenterAndFraming() {
  World world;
  CameraAnchor anchor;
  anchor.orientation = rx::QuatFromAxisAngle({1, 0, 0}, kPi * 0.5f);
  anchor.velocity = rx::Rotate(anchor.orientation, {0, 0, -1});
  Entity rig = MakeRig(world, anchor);
  world.Add(rig, CameraIntent{.recenter = true});
  world.Add(rig, CameraOrbit{.yaw = 1.0f, .space = CameraOrbitSpace::kAnchor});
  world.Add(rig, CameraRecenter{.delay = 0, .half_life = 1.0f, .minimum_speed = 0.1f});
  BuildCameraRigs(world, 1.0f);
  Near(world.Get<CameraOrbit>(rig)->yaw, 0.5f, "anchor-space recenter uses anchor-local velocity");

  World framing_world;
  CameraAnchor rolled_anchor;
  rolled_anchor.orientation = rx::QuatFromAxisAngle({0, 0, 1}, kPi * 0.5f);
  Entity framing_rig = MakeRig(framing_world, rolled_anchor);
  framing_world.Add(framing_rig, CameraBoom{.distance = 3.0f});
  framing_world.Add(framing_rig, CameraFraming{});
  StepRigs(framing_world, 0);
  const Vec3 expected_up = rx::Rotate(rolled_anchor.orientation, {0, 1, 0});
  Check(rx::Dot(CameraUp(framing_world.Get<CameraMode>(framing_rig)->view), expected_up) > 0.99f,
        "framing preserves an anchor-provided roll reference");
}

void TestFramingDampingAndDegenerateUp() {
  World world;
  Entity rig = MakeRig(world);
  world.Add(rig, CameraBoom{.distance = 3.0f});
  world.Add(rig, CameraFraming{});
  world.Add(rig, CameraDamping{.rotation_half_life = 1.0f});
  StepRigs(world, 0);

  world.Get<CameraFraming>(rig)->target_offset = {3, 0, 0};
  StepRigs(world, 1.0f);
  Near(CameraForward(world.Get<CameraMode>(rig)->view).x, 0.382683f,
       "framed orientation honors rotation half-life");

  World degenerate_world;
  CameraAnchor rolled;
  rolled.orientation = rx::QuatFromAxisAngle({0, 0, 1}, -kPi * 0.5f);
  Entity degenerate = MakeRig(degenerate_world, rolled);
  CameraFraming framing;
  framing.target_offset = {1, 0, 0};
  framing.up = {1, 0, 0};
  framing.up_space = CameraOffsetSpace::kWorld;
  degenerate_world.Add(degenerate, framing);
  StepRigs(degenerate_world, 0);
  NearVec(CameraForward(degenerate_world.Get<CameraMode>(degenerate)->view), {1, 0, 0},
          "framing handles a forward-parallel up reference");

  World normalization_world;
  CameraAnchor unnormalized;
  unnormalized.orientation = {0, 0, 0, 2};
  Entity normalized = MakeRig(normalization_world, unnormalized);
  normalization_world.Add(normalized, CameraLocalOffset{.offset = {0, 1, 0}});
  StepRigs(normalization_world, 0);
  NearVec(normalization_world.Get<CameraMode>(normalized)->view.position, {0, 1, 0},
          "anchor-local behavior normalizes source orientation");
}

}  // namespace

int main() {
  TestFirstPersonRecipe();
  TestThirdPersonRecipeAndObstruction();
  TestFrameRateIndependentDampingAndCuts();
  TestStrategyAndPlatformerRecipe();
  TestRecenterUsesShortestAngle();
  TestAnchorSpaceRecenterAndFraming();
  TestFramingDampingAndDegenerateUp();

  if (failures != 0) {
    std::fprintf(stderr, "camera_rig_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("camera_rig_test: PASS\n");
  return 0;
}
