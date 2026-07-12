// Expression controller acceptance test, pure CPU.
//
// Transitions: setting a pose converges every channel to its weight, and a
// retarget mid-flight keeps the trajectory C1 (the discrete velocity never
// jumps), which is what separates the damped springs from a lerp or a snap.
// Clamping: outputs stay in [0, 1] every step under rapid retargeting with
// the life layer on. Life layer: with a seeded RNG blinks land inside the
// configured interval window, a pose that holds the eyes closed absorbs
// them, and a fixed seed + fixed dt replays bit-identically.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "anim/expression.h"
#include "asset/asset_id.h"

using namespace rx;

namespace {

int Fail(const char* msg) {
  std::fprintf(stderr, "expression_test: FAIL: %s\n", msg);
  return 1;
}

rx::u64 Target(const char* name) { return asset::MakeAssetId(name).hash; }

anim::ExpressionController MakeController(bool life, rx::u64 seed) {
  anim::ExpressionController controller;
  controller.AddDefaultPoses();
  anim::ExpressionController::LifeConfig config;
  config.enabled = life;
  controller.set_life(config);
  controller.set_seed(seed);
  return controller;
}

// Converge to the pose weight, and stay velocity-continuous through a switch
// to another pose while the first transition is still in flight.
int TestTransition() {
  anim::ExpressionController controller = MakeController(false, 0);
  const rx::u64 smile = Target("mouthSmileLeft");
  const f32 dt = 1.0f / 240.0f;

  controller.SetExpression("smile");
  f32 previous = 0, velocity = 0, max_jump = 0;
  for (int i = 0; i < 2 * 240; ++i) {
    if (i == 36) controller.SetExpression("angry");  // mid-flight, ~0.15 s in
    if (i == 72) controller.SetExpression("smile");
    controller.Update(dt);
    const f32 weight = controller.Weight(smile);
    const f32 v = (weight - previous) / dt;
    if (i > 0) max_jump = std::max(max_jump, std::abs(v - velocity));
    previous = weight;
    velocity = v;
  }
  std::printf("expression_test: max velocity jump %g /s per step\n", max_jump);
  if (max_jump > 1.5f) return Fail("velocity discontinuity across a retarget");
  if (std::abs(controller.Weight(smile) - 0.75f) > 1e-3f) {
    return Fail("smile did not converge to its pose weight");
  }
  if (std::abs(velocity) > 1e-2f) return Fail("velocity did not settle");

  controller.SetExpression("neutral");
  for (int i = 0; i < 2 * 240; ++i) controller.Update(dt);
  if (std::abs(controller.Weight(smile)) > 1e-3f) return Fail("neutral did not release");
  return 0;
}

// Every output stays in [0, 1] every step, even while rapid retargets with a
// short transition time keep every channel mid-flight and the life layer on.
int TestClamped() {
  anim::ExpressionController controller = MakeController(true, 1);
  const char* cycle[] = {"smile", "angry", "surprised", "eyes_closed", "pucker", "smirk",
                         "neutral"};
  const f32 dt = 1.0f / 120.0f;
  int pose = 0;
  for (int i = 0; i < 10 * 120; ++i) {
    if (i % 6 == 0) controller.SetExpression(cycle[pose++ % 7], 0.1f);
    controller.Update(dt);
    for (u32 c = 0; c < controller.channel_count(); ++c) {
      const f32 w = controller.channel_weight(c);
      if (!(w >= 0.0f && w <= 1.0f)) return Fail("weight escaped [0, 1] mid-transition");
    }
  }
  return 0;
}

// Blinks fire inside the configured interval window, and a pose that already
// holds the eyes closed absorbs them (no visible dip or double-close).
int TestBlink() {
  anim::ExpressionController controller = MakeController(true, 42);
  const rx::u64 blink = Target("eyeBlinkLeft");
  const f32 dt = 1.0f / 120.0f;

  controller.SetExpression("neutral");
  int blinks = 0;
  f32 time = 0, last_start = 0, max_gap = 0;
  bool closed = false;
  for (int i = 0; i < 30 * 120; ++i) {
    controller.Update(dt);
    time += dt;
    const bool now = controller.Weight(blink) > 0.5f;
    if (now && !closed) {
      if (blinks > 0) max_gap = std::max(max_gap, time - last_start);
      last_start = time;
      ++blinks;
    }
    closed = now;
  }
  std::printf("expression_test: %d blinks in 30 s, max gap %.2f s\n", blinks, max_gap);
  // 2-6 s intervals plus occasional double blinks: 5..25 in 30 s.
  if (blinks < 5 || blinks > 25) return Fail("blink count outside the interval window");
  if (max_gap > 6.6f) return Fail("blink gap exceeded the configured maximum");

  controller.SetExpression("eyes_closed");
  for (int i = 0; i < 2 * 120; ++i) controller.Update(dt);
  for (int i = 0; i < 10 * 120; ++i) {
    controller.Update(dt);
    if (controller.Weight(blink) < 0.98f) return Fail("blink not absorbed by closed eyes");
  }
  return 0;
}

// Same seed + same dt sequence replays bit-identically; a different seed
// diverges (the blink schedule moves).
int TestDeterminism() {
  anim::ExpressionController a = MakeController(true, 7);
  anim::ExpressionController b = MakeController(true, 7);
  anim::ExpressionController c = MakeController(true, 8);
  const f32 dt = 1.0f / 60.0f;
  bool diverged = false;
  for (int i = 0; i < 12 * 60; ++i) {
    if (i == 60) {
      a.SetExpression("smile");
      b.SetExpression("smile");
      c.SetExpression("smile");
    }
    a.Update(dt);
    b.Update(dt);
    c.Update(dt);
    for (u32 ch = 0; ch < a.channel_count(); ++ch) {
      const f32 wa = a.channel_weight(ch);
      const f32 wb = b.channel_weight(ch);
      if (std::memcmp(&wa, &wb, sizeof(f32)) != 0) return Fail("same seed diverged");
      if (wa != c.channel_weight(ch)) diverged = true;
    }
  }
  if (!diverged) return Fail("different seeds never diverged");
  return 0;
}

}  // namespace

int main() {
  if (int rc = TestTransition()) return rc;
  if (int rc = TestClamped()) return rc;
  if (int rc = TestBlink()) return rc;
  if (int rc = TestDeterminism()) return rc;
  std::printf("expression_test: PASS\n");
  return 0;
}
