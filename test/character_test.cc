#include "character/character.h"

#include <cmath>
#include <cstdio>

#include "ecs/world.h"
#include "physics/physics_world.h"
#include "scene/camera.h"
#include "scene/camera_rig.h"
#include "scene/components.h"

using namespace rx;
using namespace rx::character;
namespace ecs = rx::ecs;
namespace scene = rx::scene;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "character_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-2f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "character_test: FAIL: %s (got %.4f, expected %.4f)\n", message, actual,
               expected);
  ++failures;
}

// A character standing on a large floor. Feet start slightly high so the first
// steps settle it onto the ground.
struct Scene {
  physics::PhysicsWorld physics;
  ecs::World world;
  ecs::Entity player{};

  bool Init(const Vec3& feet = {0, 0.05f, 0}) {
    if (!physics.Initialize()) return false;
    physics.AddStaticBox({0, -0.5f, 0}, {60, 0.5f, 60});  // top at y = 0

    CharacterShape shape;  // defaults: standing 1.8/0.3, crouched 1.2/0.3
    const f32 half = std::max(shape.standing_height * 0.5f - shape.standing_radius, 0.01f);
    const f32 total_half = half + shape.standing_radius;
    const Vec3 center = feet + Vec3{0, total_half, 0};
    physics::CharacterId id = physics.CreateCharacter(center, shape.standing_radius, half);

    player = world.Create();
    world.Add(player, CharacterMovementSettings{});
    world.Add(player, shape);
    world.Add(player, CharacterIntent{});
    CharacterState state;
    state.eye_height = shape.standing_eye_height;
    world.Add(player, state);
    world.Add(player, CharacterBody{id, shape.standing_radius, half, false});
    scene::Transform t;
    t.position[0] = feet.x;
    t.position[1] = feet.y;
    t.position[2] = feet.z;
    world.Add(player, t);
    physics.Update(kDt);  // build the broadphase before the first query
    return true;
  }

  CharacterIntent& intent() { return *world.Get<CharacterIntent>(player); }
  CharacterState& state() { return *world.Get<CharacterState>(player); }
  CharacterBody& body() { return *world.Get<CharacterBody>(player); }
  scene::Transform& transform() { return *world.Get<scene::Transform>(player); }

  void Step(int count) {
    for (int i = 0; i < count; ++i) {
      StepCharacters(world, physics, kDt);
      physics.Update(kDt);
    }
  }

  void Settle() { Step(30); }
};

f32 HorizontalSpeed(const CharacterState& s) {
  return std::sqrt(s.velocity.x * s.velocity.x + s.velocity.z * s.velocity.z);
}

void TestGaitSpeeds() {
  Scene s;
  if (!s.Init()) {
    Check(false, "physics init (Jolt missing?)");
    return;
  }
  s.Settle();

  s.intent().move = {0, 0, -1};
  s.intent().gait = CharacterGait::kWalk;
  s.Step(120);
  Near(HorizontalSpeed(s.state()), CharacterMovementSettings{}.walk_speed,
       "walk gait reaches walk speed", 0.05f);

  s.intent().gait = CharacterGait::kSprint;
  s.Step(120);
  Near(HorizontalSpeed(s.state()), CharacterMovementSettings{}.sprint_speed,
       "sprint gait reaches sprint speed", 0.1f);

  s.intent().move = {0, 0, 0};
  s.Step(120);
  Near(HorizontalSpeed(s.state()), 0.0f, "releasing input decelerates to rest", 0.05f);
}

void TestCrouchBlend() {
  Scene s;
  if (!s.Init()) return;
  s.Settle();

  const f32 standing_half = s.body().half_height;
  s.intent().crouch = true;
  s.Step(60);
  Check(s.state().stance == CharacterStance::kCrouching, "crouch enters crouching stance");
  Near(s.state().crouch_blend, 1.0f, "crouch blend saturates");
  Check(s.body().half_height < standing_half - 0.1f, "crouch shrinks the capsule");
  Near(s.state().eye_height, CharacterShape{}.crouched_eye_height, "eye height blends to crouched",
       0.03f);

  s.intent().crouch = false;
  s.Step(60);
  Check(s.state().stance == CharacterStance::kStanding, "uncrouch stands back up in the open");
  Near(s.state().eye_height, CharacterShape{}.standing_eye_height, "eye height blends to standing",
       0.03f);
}

void TestUncrouchUnderCeiling() {
  Scene s;
  if (!s.Init()) return;
  // Ceiling bottom at y = 1.5: clears a crouched (1.2) head, blocks a standing
  // (1.8) one. Only over x in [-2, 2].
  s.physics.AddStaticBox({0, 1.6f, 0}, {2, 0.1f, 2});
  s.Settle();

  s.intent().crouch = true;
  s.Step(60);
  Check(s.state().stance == CharacterStance::kCrouching, "crouched under the ceiling");

  s.intent().crouch = false;
  s.Step(60);
  Check(s.state().stance == CharacterStance::kCrouching, "uncrouch refused with no headroom");

  // Move clear of the ceiling, then try again.
  TeleportCharacter(s.world, s.physics, s.player, {5, 0.05f, 0});
  s.Settle();
  s.intent().crouch = false;
  s.Step(60);
  Check(s.state().stance == CharacterStance::kStanding, "uncrouch succeeds once clear");
}

void TestJumpAndLand() {
  Scene s;
  if (!s.Init()) return;
  s.Settle();
  Check(s.state().grounded, "settled character is grounded");
  const f32 rest_y = s.transform().position[1];

  s.intent().jump = true;
  s.Step(1);
  Check(!s.state().grounded, "character leaves the ground after jumping");
  Check(s.state().velocity.y > 0.0f, "jump imparts upward velocity");

  bool rose = false;
  for (int i = 0; i < 240 && !s.state().grounded; ++i) {
    if (s.transform().position[1] > rest_y + 0.3f) rose = true;
    s.Step(1);
  }
  Check(rose, "character rises during the jump arc");
  Check(s.state().grounded, "character lands and regrounds");
  Near(s.transform().position[1], rest_y, "character returns to ground height", 0.05f);
}

void TestStepOverAndBlocked() {
  {
    Scene s;
    if (!s.Init()) return;
    // A 0.4 m step across the forward (-Z) path; top at y = 0.4.
    s.physics.AddStaticBox({0, 0.2f, -1.5f}, {1.5f, 0.2f, 0.5f});
    s.Settle();
    s.intent().move = {0, 0, -1};
    s.intent().gait = CharacterGait::kRun;
    s.Step(200);
    Check(s.transform().position[2] < -1.8f, "controller steps over a step-height ledge");
  }
  {
    Scene s;
    if (!s.Init()) return;
    // A 1.2 m wall across the path; taller than the step height.
    s.physics.AddStaticBox({0, 0.6f, -1.5f}, {1.5f, 0.6f, 0.5f});
    s.Settle();
    s.intent().move = {0, 0, -1};
    s.intent().gait = CharacterGait::kRun;
    s.Step(200);
    Check(s.transform().position[2] > -1.2f, "controller is blocked by a tall wall");
  }
}

void TestCameraObstruction() {
  physics::PhysicsWorld physics;
  if (!physics.Initialize()) return;
  // Wall between the pivot (z=0) and the third-person boom target (z=+3).
  physics.AddStaticBox({0, 1.5f, 2.0f}, {3, 3, 0.2f});  // front face at z = 1.8
  physics.Update(kDt);

  ecs::World world;
  ecs::Entity rig = world.Create();
  world.Add(rig, scene::CameraAnchor{.position = {0, 1.5f, 0}});
  world.Add(rig, scene::CameraRigPose{});
  world.Add(rig, scene::CameraMode{});
  world.Add(rig, scene::CameraOrbit{.space = scene::CameraOrbitSpace::kAnchor});
  world.Add(rig, scene::CameraBoom{.distance = 3.0f});
  world.Add(rig, scene::CameraObstruction{.radius = 0.25f, .margin = 0.1f});

  scene::BuildCameraRigs(world, kDt);
  scene::PrepareCameraRigConstraints(world, kDt);
  AnswerCameraObstructions(world, physics);
  scene::ResolveCameraRigs(world, kDt);

  scene::CameraObstruction* o = world.Get<scene::CameraObstruction>(rig);
  Check(o->obstructed, "obstruction sphere cast reports a hit");
  scene::CameraMode* mode = world.Get<scene::CameraMode>(rig);
  Check(mode->view.position.z < 1.8f, "third-person camera is pulled in front of the wall");
  Check(mode->view.position.z > 0.5f, "camera does not overshoot past the pivot");
}

void TestFirstPersonEyeFollowsCrouch() {
  Scene s;
  if (!s.Init()) return;
  s.world.Add(s.player, CharacterViewMode{});  // defaults to first person
  ApplyCharacterViewMode(s.world, s.player);
  s.Settle();

  s.intent().crouch = true;
  for (int i = 0; i < 60; ++i) {
    StepCharacters(s.world, s.physics, kDt);
    s.physics.Update(kDt);
    SyncCharacterCameraAnchors(s.world);
    scene::BuildCameraRigs(s.world, kDt);
    scene::PrepareCameraRigConstraints(s.world, kDt);
    scene::ResolveCameraRigs(s.world, kDt);
  }
  scene::CameraMode* mode = s.world.Get<scene::CameraMode>(s.player);
  Near(mode->view.position.y, CharacterShape{}.crouched_eye_height,
       "first-person eye tracks the crouched eye height", 0.05f);
}

void TestViewModeToggleTransition() {
  Scene s;
  if (!s.Init()) return;
  s.world.Add(s.player, CharacterViewMode{});
  ApplyCharacterViewMode(s.world, s.player);
  s.Settle();

  // Stack output with the player as its base camera mode.
  ecs::Entity output = s.world.Create();
  scene::CameraStackResult init = scene::InitializeCameraStack(s.world, output, s.player);
  Check(init == scene::CameraStackResult::kSuccess, "camera stack initializes on the player mode");

  SyncCharacterCameraAnchors(s.world);
  scene::BuildCameraRigs(s.world, kDt);
  scene::PrepareCameraRigConstraints(s.world, kDt);
  scene::ResolveCameraRigs(s.world, kDt);
  scene::ResolveCameraStacks(s.world, kDt);

  const CharacterViewKind before = s.world.Get<CharacterViewMode>(s.player)->kind;
  scene::CameraStackResult r = ToggleCharacterViewMode(s.world, s.player, output, s.player, {},
                                                       {.duration = 0.3f});
  Check(r == scene::CameraStackResult::kSuccess, "view-mode toggle pushes onto the stack");
  Check(s.world.Get<CharacterViewMode>(s.player)->kind != before, "view kind flips FP<->TP");
  Check(s.world.Get<scene::CameraStack>(output)->transition.active,
        "view-mode toggle drives a camera-stack transition");
  Check(s.world.Has<scene::CameraBoom>(s.player), "third-person recipe installs a boom");
}

}  // namespace

int main() {
  TestGaitSpeeds();
  TestCrouchBlend();
  TestUncrouchUnderCeiling();
  TestJumpAndLand();
  TestStepOverAndBlocked();
  TestCameraObstruction();
  TestFirstPersonEyeFollowsCrouch();
  TestViewModeToggleTransition();

  if (failures == 0) {
    std::printf("character_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "character_test: %d checks failed\n", failures);
  return 1;
}
