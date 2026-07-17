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

void TestAnalogGait() {
  Scene s;
  if (!s.Init()) return;
  s.Settle();

  // Half stick deflection walks at half the gait target speed (continuous).
  s.intent().move = {0, 0, -0.5f};
  s.intent().gait = CharacterGait::kRun;
  s.Step(120);
  Near(HorizontalSpeed(s.state()), CharacterMovementSettings{}.run_speed * 0.5f,
       "half-deflected move throttles to half speed", 0.05f);

  // Full deflection returns to full run speed.
  s.intent().move = {0, 0, -1.0f};
  s.Step(120);
  Near(HorizontalSpeed(s.state()), CharacterMovementSettings{}.run_speed,
       "full deflection reaches full speed", 0.05f);
}

void TestGaitSpeedBlend() {
  Scene s;
  if (!s.Init()) return;
  s.Settle();

  // At a full run, the smoothed gait target speed sits at run speed.
  s.intent().move = {0, 0, -1};
  s.intent().gait = CharacterGait::kRun;
  s.Step(120);
  Near(s.state().gait_speed, CharacterMovementSettings{}.run_speed, "gait speed settles at run",
       0.05f);

  // Switch to sprint: the target speed must climb monotonically (no step) and
  // reach sprint speed, not jump to it in one frame.
  s.intent().gait = CharacterGait::kSprint;
  f32 prev = s.state().gait_speed;
  bool monotonic = true, stepped = false;
  const f32 run = CharacterMovementSettings{}.run_speed;
  const f32 sprint = CharacterMovementSettings{}.sprint_speed;
  for (int i = 0; i < 60; ++i) {
    StepCharacters(s.world, s.physics, kDt);
    s.physics.Update(kDt);
    const f32 g = s.state().gait_speed;
    if (g < prev - 1e-4f) monotonic = false;
    if (i == 0 && g >= sprint - 1e-3f) stepped = true;  // reached sprint in one frame == stepped
    prev = g;
  }
  Check(monotonic, "gait target speed blends monotonically upward");
  Check(!stepped, "gait target speed does not step to sprint in one frame");
  Near(s.state().gait_speed, sprint, "gait target speed reaches sprint", 0.05f);
  (void)run;
}

void TestCrispStop() {
  Scene s;
  if (!s.Init()) return;
  s.Settle();
  s.intent().move = {0, 0, -1};
  s.intent().gait = CharacterGait::kSprint;
  s.Step(120);
  Check(HorizontalSpeed(s.state()) > 5.0f, "sprinting before the stop");

  s.intent().move = {0, 0, 0};
  s.Step(60);
  Check(s.state().velocity.x == 0.0f && s.state().velocity.z == 0.0f,
        "crisp stop: horizontal velocity is exactly zero (no ice-skate tail)");
}

void TestTurnSmoothingConverges() {
  Scene s;
  if (!s.Init()) return;
  CharacterViewMode vm;
  vm.kind = CharacterViewKind::kThirdPerson;  // third person eases facing to movement dir
  s.world.Add(s.player, vm);
  auto* move = s.world.Get<CharacterMovementSettings>(s.player);
  move->turn_half_life = 0.1f;
  s.Settle();

  // Movement direction with heading yaw ~1.0 rad.
  const f32 theta = 1.0f;
  s.intent().move = {std::sin(theta), 0, -std::cos(theta)};
  s.intent().gait = CharacterGait::kRun;

  f32 prev = s.state().facing_yaw;
  bool monotonic = true, overshoot = false;
  for (int i = 0; i < 300; ++i) {
    StepCharacters(s.world, s.physics, kDt);
    s.physics.Update(kDt);
    const f32 f = s.state().facing_yaw;
    if (f < prev - 1e-4f) monotonic = false;   // only turns toward the target
    if (f > theta + 2e-3f) overshoot = true;    // never past it
    prev = f;
  }
  Check(monotonic, "facing turns monotonically toward the movement direction");
  Check(!overshoot, "facing does not overshoot the movement direction");
  Near(s.state().facing_yaw, theta, "facing converges to the movement direction", 0.02f);
}

void TestQuickPivotIsFaster() {
  // Same large (near-reversal) target under an enabled pivot vs a disabled one;
  // the pivot must close much more of the turn in the same time.
  auto facing_after = [](f32 pivot_angle, int steps) -> f32 {
    Scene s;
    if (!s.Init()) return 0;
    CharacterViewMode vm;
    vm.kind = CharacterViewKind::kThirdPerson;
    s.world.Add(s.player, vm);
    auto* move = s.world.Get<CharacterMovementSettings>(s.player);
    move->turn_half_life = 0.5f;        // slow ordinary turn
    move->pivot_turn_half_life = 0.03f;  // fast pivot
    move->pivot_angle = pivot_angle;
    s.Settle();
    const f32 theta = 2.6f;  // ~149 deg from the +? forward: a near reversal
    s.intent().move = {std::sin(theta), 0, -std::cos(theta)};
    s.intent().gait = CharacterGait::kRun;
    for (int i = 0; i < steps; ++i) {
      StepCharacters(s.world, s.physics, kDt);
      s.physics.Update(kDt);
    }
    return s.state().facing_yaw;
  };
  const f32 with_pivot = facing_after(2.0f, 3);    // 2.6 > 2.0 -> pivot latches
  const f32 without_pivot = facing_after(10.0f, 3);  // never pivots -> slow
  Check(with_pivot > without_pivot + 0.3f,
        "a near-180 reversal pivots faster than an ordinary turn");
}

void TestJumpBufferOnLanding() {
  Scene s;
  if (!s.Init()) return;
  s.Settle();
  Check(s.state().grounded, "grounded before the jump");

  s.intent().jump = true;  // first jump
  s.Step(1);
  Check(!s.state().grounded, "airborne after the first jump");

  // While descending near the ground, buffer a jump ONE time and stop pressing.
  bool primed = false, rejumped = false;
  for (int i = 0; i < 400; ++i) {
    if (!primed && !s.state().grounded && s.state().velocity.y < 0 &&
        s.transform().position[1] < 0.25f) {
      s.intent().jump = true;
      primed = true;
    }
    s.Step(1);
    if (primed && s.state().velocity.y > 1.0f) {  // a fresh upward launch == buffered jump fired
      rejumped = true;
      break;
    }
  }
  Check(rejumped, "a jump buffered before touchdown fires on landing");
}

void TestCoyoteJump() {
  // Positive: a jump within the coyote window after walking off a ledge fires.
  {
    Scene s;
    if (!s.Init()) return;
    s.physics.AddStaticBox({-1.0f, 1.0f, 0}, {1.0f, 1.0f, 3.0f});  // top at y=2, x in [-2,0]
    TeleportCharacter(s.world, s.physics, s.player, {-0.8f, 2.0f, 0});
    s.Settle();
    Check(s.state().grounded, "standing on the ledge");

    s.intent().move = {1, 0, 0};  // walk +x off the edge (edge at x=0)
    s.intent().gait = CharacterGait::kRun;
    int i = 0;
    for (; i < 180 && s.state().grounded; ++i) s.Step(1);
    Check(!s.state().grounded, "walked off the ledge into the air");
    Check(s.state().time_since_grounded <= CharacterMovementSettings{}.coyote_time,
          "still inside the coyote window right after leaving");

    s.intent().jump = true;
    s.Step(1);
    Check(s.state().velocity.y > 0.0f, "coyote jump fires within the window");
  }
  // Negative: past the window the jump is refused.
  {
    Scene s;
    if (!s.Init()) return;
    s.physics.AddStaticBox({-1.0f, 1.0f, 0}, {1.0f, 1.0f, 3.0f});
    TeleportCharacter(s.world, s.physics, s.player, {-0.8f, 2.0f, 0});
    s.Settle();
    s.intent().move = {1, 0, 0};
    s.intent().gait = CharacterGait::kRun;
    for (int i = 0; i < 180 && s.state().grounded; ++i) s.Step(1);
    Check(!s.state().grounded, "walked off the ledge (negative case)");
    // Let the coyote window lapse while falling.
    for (int i = 0; i < 20; ++i) s.Step(1);
    Check(s.state().time_since_grounded > CharacterMovementSettings{}.coyote_time,
          "coyote window has lapsed");
    Check(!s.state().grounded, "still airborne past the window");
    s.intent().jump = true;
    s.Step(1);
    Check(s.state().velocity.y < 0.0f, "jump refused past the coyote window (still falling)");
  }
}

void TestAnchorVerticalSmoothing() {
  Scene s;
  if (!s.Init()) return;
  auto* shape = s.world.Get<CharacterShape>(s.player);
  shape->eye_step_half_life = 0.15f;  // exaggerate so the lag is unambiguous
  // A 0.30 m step up onto a deep raised platform (top at y = 0.30, within step
  // height) that the character climbs onto and stays on.
  s.physics.AddStaticBox({0, 0.15f, -10.0f}, {1.5f, 0.15f, 9.0f});  // top y=0.30, z in [-19,-1]
  s.world.Add(s.player, scene::CameraAnchor{});
  s.Settle();

  s.intent().move = {0, 0, -1};
  s.intent().gait = CharacterGait::kRun;
  f32 max_lag = 0;
  for (int i = 0; i < 200; ++i) {
    StepCharacters(s.world, s.physics, kDt);
    s.physics.Update(kDt);
    SyncCharacterCameraAnchors(s.world);
    const CharacterState& st = s.state();
    const f32 raw_eye = s.transform().position[1] + st.eye_height;
    max_lag = std::max(max_lag, raw_eye - st.anchor_eye_y);  // smoothed lags a rising step
  }
  Check(s.transform().position[1] > 0.25f, "character climbed the step");
  Check(max_lag > 0.05f, "anchor eye Y lags a sudden step-up (glide, not pop)");
  // After settling on the step, the smoothed eye converges back to the raw eye.
  s.intent().move = {0, 0, 0};
  s.Step(60);
  const f32 raw_eye = s.transform().position[1] + s.state().eye_height;
  Near(s.state().anchor_eye_y, raw_eye, "anchor eye Y converges once the step is done", 0.01f);
}

void TestLandingDip() {
  Scene s;
  if (!s.Init()) return;
  s.Settle();
  Near(s.state().landing_dip, 0.0f, "no dip while standing", 1e-3f);

  s.intent().jump = true;
  s.Step(1);
  // Fall back down; capture the dip at touchdown.
  f32 peak_dip = 0;
  bool landed = false;
  for (int i = 0; i < 400; ++i) {
    s.Step(1);
    peak_dip = std::max(peak_dip, s.state().landing_dip);
    if (s.state().grounded && i > 2) {
      landed = true;
      break;
    }
  }
  Check(landed, "character lands");
  Check(peak_dip > 0.0f, "landing after a fall dips the eye height");
  // The dip is capped subtle and recovers.
  Check(peak_dip <= CharacterShape{}.landing_dip_max + 1e-3f, "landing dip is capped");
  s.Step(60);
  Near(s.state().landing_dip, 0.0f, "landing dip recovers to zero", 5e-3f);
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

  // Regression (F1): each toggle retires the previous pushed mode by writing
  // the freshly re-acquired CharacterViewMode::transition. Before the fix that
  // write went through a component pointer invalidated by the toggle's own
  // add/remove churn, so the live activation stayed empty, the next toggle
  // could never release the prior push, and CameraStack::entries grew without
  // bound. Drive many toggles, settling the transition each time, and assert
  // the stack stays bounded rather than growing per toggle.
  auto resolve_frame = [&] {
    SyncCharacterCameraAnchors(s.world);
    scene::BuildCameraRigs(s.world, kDt);
    scene::PrepareCameraRigConstraints(s.world, kDt);
    scene::ResolveCameraRigs(s.world, kDt);
    scene::ResolveCameraStacks(s.world, kDt);
  };
  auto settle_stack = [&] {
    for (int i = 0; i < 60; ++i) resolve_frame();  // > the 0.3s transition
  };
  settle_stack();
  const size_t bounded = s.world.Get<scene::CameraStack>(output)->entries.size();
  for (int i = 0; i < 12; ++i) {
    scene::CameraStackResult tr = ToggleCharacterViewMode(s.world, s.player, output,
                                                          s.player, {}, {.duration = 0.3f});
    Check(tr == scene::CameraStackResult::kSuccess, "repeated view-mode toggle succeeds");
    settle_stack();
    Check(s.world.Get<scene::CameraStack>(output)->entries.size() == bounded,
          "camera stack stays bounded across repeated toggles");
  }
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
  TestAnalogGait();
  TestGaitSpeedBlend();
  TestCrispStop();
  TestTurnSmoothingConverges();
  TestQuickPivotIsFaster();
  TestJumpBufferOnLanding();
  TestCoyoteJump();
  TestAnchorVerticalSmoothing();
  TestLandingDip();
  TestViewModeToggleTransition();

  if (failures == 0) {
    std::printf("character_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "character_test: %d checks failed\n", failures);
  return 1;
}
