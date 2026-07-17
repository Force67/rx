// Headless proof of the player jetpack (engine/character/jetpack.{h,cc}) over the
// real Jolt character controller. Needs the Jolt-enabled physics + character
// build (like character_test). Each lettered scenario prints its measured
// numbers to stderr and returns Fail(...) on the first bad assert.
//
//   (a) full burn from rest: leaves the ground and climbs a sane amount.
//   (b) fuel drains to empty in ~capacity s, thrust dies, the character falls.
//   (c) grounded refuel restores fuel; airborne does not.
//   (d) spool lag: thrust reaches 90% only after ~spool_time.
//   (e) airborne WASD accelerates horizontally faster burning than in free fall.
//   (f) NaN-free under random input, and the plain jump still works pack-off.

#include <cmath>
#include <cstdio>

#include "character/character.h"
#include "character/jetpack.h"
#include "core/math.h"
#include "ecs/world.h"
#include "physics/physics_world.h"
#include "scene/components.h"

using namespace rx;
using namespace rx::character;
namespace ecs = rx::ecs;
namespace scene = rx::scene;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

int Fail(const char* what) {
  std::fprintf(stderr, "jetpack_test FAIL: %s\n", what);
  return 1;
}

// Character + jetpack on flat ground, stepped the way the gym stages it:
// StepJetpacks BEFORE StepCharacters, then advance the physics world.
struct Scene {
  physics::PhysicsWorld physics;
  ecs::World world;
  ecs::Entity player{};

  bool Init(const JetpackDesc& jp = {}, const Vec3& feet = {0, 0.05f, 0}) {
    if (!physics.Initialize()) return false;
    physics.AddStaticBox({0, -0.5f, 0}, {200, 0.5f, 200});  // ground top at y = 0

    CharacterShape shape;
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
    world.Add(player, jp);
    world.Add(player, JetpackInput{});
    world.Add(player, JetpackState{});
    physics.Update(kDt);  // build the broadphase
    return true;
  }

  CharacterIntent& intent() { return *world.Get<CharacterIntent>(player); }
  CharacterState& state() { return *world.Get<CharacterState>(player); }
  JetpackInput& jin() { return *world.Get<JetpackInput>(player); }
  JetpackState& jst() { return *world.Get<JetpackState>(player); }
  scene::Transform& transform() { return *world.Get<scene::Transform>(player); }
  f32 feet_y() { return transform().position[1]; }

  void Step(int count) {
    for (int i = 0; i < count; ++i) {
      StepJetpacks(world, kDt);
      StepCharacters(world, physics, kDt);
      physics.Update(kDt);
    }
  }
};

f32 HSpeed(const Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }

}  // namespace

int main() {
  {
    physics::PhysicsWorld probe;
    if (!probe.Initialize()) {
      std::fprintf(stderr, "jetpack_test: physics stub, skipping\n");
      return 0;
    }
  }

  // (a) full burn from rest: leaves the ground, climbs a sane amount -----------
  {
    Scene s;
    if (!s.Init()) return Fail("(a) init");
    s.Step(20);  // settle on the ground
    const f32 y0 = s.feet_y();
    s.jin().enabled = true;
    s.jin().thrust = true;
    f32 peak = y0;
    for (int i = 0; i < 60 * 3; ++i) {  // 3 s of burn
      s.jin().enabled = true;
      s.jin().thrust = true;
      s.Step(1);
      peak = std::max(peak, s.feet_y());
    }
    const bool airborne = !s.state().grounded;
    std::fprintf(stderr, "(a) climb: y0=%.2f peak=%.2f gain=%.2f m  airborne=%d  fuel=%.2f\n", y0,
                 peak, peak - y0, airborne, s.jst().fuel);
    if (peak - y0 < 1.5f) return Fail("(a) jetpack did not lift the character off");
    if (peak - y0 > 80.0f) return Fail("(a) climb implausibly large");
  }

  // (b) fuel drains to empty in ~capacity s and thrust dies -> descend ---------
  {
    JetpackDesc jp;  // capacity 4 s, TWR 1.45
    Scene s;
    if (!s.Init(jp)) return Fail("(b) init");
    s.Step(20);
    int empty_step = -1;
    f32 peak = s.feet_y();
    for (int i = 0; i < 60 * 8; ++i) {
      s.jin().enabled = true;
      s.jin().thrust = true;
      s.Step(1);
      peak = std::max(peak, s.feet_y());
      if (empty_step < 0 && s.jst().fuel <= 0.0f) empty_step = i;
    }
    const f32 t_empty = empty_step / 60.0f;
    // After the tank is dry, thrust must have died and the character fallen.
    const f32 y_after = s.feet_y();
    std::fprintf(stderr, "(b) empty at %.2f s (cap %.1f)  thrust=%.2f  peak=%.2f y_after=%.2f\n",
                 t_empty, jp.fuel_capacity_s, s.jst().thrust, peak, y_after);
    if (empty_step < 0) return Fail("(b) tank never emptied");
    if (t_empty < jp.fuel_capacity_s * 0.8f || t_empty > jp.fuel_capacity_s * 1.6f)
      return Fail("(b) tank did not empty in ~capacity seconds");
    if (s.jst().thrust > 0.05f) return Fail("(b) thrust did not die on empty");
    if (y_after > peak - 1.0f) return Fail("(b) character did not descend after dead stick");
  }

  // (c) grounded refuel restores fuel; airborne does not -----------------------
  {
    Scene s;
    if (!s.Init()) return Fail("(c) init");
    s.Step(20);
    // Drain most of the tank with a burst that keeps us near the ground.
    s.jst().fuel = 0.2f;
    for (int i = 0; i < 30; ++i) {
      s.jin().enabled = true;
      s.jin().thrust = true;
      s.Step(1);
    }
    const f32 fuel_low = s.jst().fuel;
    // Sit grounded, idle: should refuel.
    s.jin().thrust = false;
    for (int i = 0; i < 60 * 3; ++i) s.Step(1);
    const f32 fuel_grounded = s.jst().fuel;
    // Now go airborne with a plain jump (pack off), get clear of the ground,
    // THEN park fuel mid-tank and confirm airborne flight does not refuel it.
    s.jin().enabled = false;
    s.intent().jump = true;
    s.Step(1);
    for (int i = 0; i < 6 && !s.state().grounded; ++i) s.Step(1);  // rise clear
    if (s.state().grounded) return Fail("(c) never got airborne to test");
    s.jst().fuel = 0.5f;  // park mid-tank while airborne
    f32 fuel_air = 0.5f;
    for (int i = 0; i < 8 && !s.state().grounded; ++i) {
      s.Step(1);
      fuel_air = s.jst().fuel;
    }
    std::fprintf(stderr, "(c) fuel low=%.2f -> grounded=%.2f ; airborne stays=%.2f\n", fuel_low,
                 fuel_grounded, fuel_air);
    if (fuel_grounded <= fuel_low + 0.05f) return Fail("(c) grounded refuel did not restore fuel");
    if (fuel_air > 0.5f + 1e-3f) return Fail("(c) airborne wrongly refuelled");
  }

  // (d) spool lag: thrust reaches 90% only after ~spool_time -------------------
  {
    JetpackDesc jp;
    jp.spool_time = 0.3f;
    Scene s;
    if (!s.Init(jp)) return Fail("(d) init");
    s.Step(20);
    // Sample the thrust curve while holding full demand.
    const int n_early = static_cast<int>((jp.spool_time / 3.0f) * 60.0f);
    const int n_spool = static_cast<int>(jp.spool_time * 60.0f);
    f32 thrust_early = 0.0f, thrust_spool = 0.0f;
    for (int i = 0; i < n_spool + 1; ++i) {
      s.jin().enabled = true;
      s.jin().thrust = true;
      s.Step(1);
      if (i == n_early) thrust_early = s.jst().thrust;
      if (i == n_spool) thrust_spool = s.jst().thrust;
    }
    std::fprintf(stderr, "(d) thrust @ t/3=%.2f  @ spool_time=%.2f (target 0.90)\n", thrust_early,
                 thrust_spool);
    if (thrust_early > 0.75f) return Fail("(d) thrust rose too fast (no spool lag)");
    if (thrust_spool < 0.82f || thrust_spool > 0.97f)
      return Fail("(d) thrust did not reach ~90% at spool_time");
  }

  // (e) airborne WASD accelerates faster burning than in free fall -------------
  {
    // Common launch: jump straight up, then apply +X move for a fixed window,
    // once with the pack burning and once free-falling. Compare horizontal speed.
    auto lateral_gain = [](bool burn) -> f32 {
      Scene s;
      s.Init();
      s.Step(20);
      s.intent().jump = true;
      s.Step(1);
      for (int i = 0; i < 8; ++i) s.Step(1);  // rise clear of the ground
      s.intent().move = {1, 0, 0};            // world +X
      for (int i = 0; i < 30; ++i) {
        s.intent().move = {1, 0, 0};
        s.jin().enabled = burn;
        s.jin().thrust = burn;
        s.Step(1);
      }
      return HSpeed(s.state().velocity);
    };
    const f32 drift = lateral_gain(false);
    const f32 powered = lateral_gain(true);
    std::fprintf(stderr, "(e) lateral speed  free-fall=%.2f  burning=%.2f m/s\n", drift, powered);
    if (powered <= drift + 0.5f)
      return Fail("(e) burning did not out-accelerate free-fall drift horizontally");
  }

  // (f) NaN-free under random input; plain jump still works pack-off -----------
  {
    Scene s;
    if (!s.Init()) return Fail("(f) init");
    s.Step(20);
    for (int i = 0; i < 60 * 20; ++i) {
      const f32 t = i * kDt;
      s.jin().enabled = std::sin(t * 3.1f) > 0.0f;
      s.jin().thrust = std::sin(t * 7.3f) > 0.0f;
      s.intent().move = {std::sin(t * 2.0f), 0, std::cos(t * 1.3f)};
      if (std::sin(t * 5.0f) > 0.9f) s.intent().jump = true;
      s.Step(1);
      if (!std::isfinite(s.feet_y()) || !std::isfinite(s.jst().fuel) ||
          !std::isfinite(s.state().velocity.y))
        return Fail("(f) NaN under random input");
    }
    std::fprintf(stderr, "(f) random 20 s: finite, fuel=%.2f y=%.2f\n", s.jst().fuel, s.feet_y());

    // Pack OFF: a plain jump must still leave the ground and come back.
    Scene j;
    if (!j.Init()) return Fail("(f) jump init");
    j.Step(30);
    const f32 ground_y = j.feet_y();
    j.jin().enabled = false;
    j.intent().jump = true;
    f32 jump_peak = ground_y;
    for (int i = 0; i < 90; ++i) {
      j.Step(1);
      jump_peak = std::max(jump_peak, j.feet_y());
    }
    std::fprintf(stderr, "(f) pack-off jump: ground=%.2f peak=%.2f (height ~%.2f m)\n", ground_y,
                 jump_peak, jump_peak - ground_y);
    if (jump_peak - ground_y < 0.6f) return Fail("(f) plain jump broken with pack off");
    if (!j.state().grounded) return Fail("(f) character never landed after the jump");
  }

  // (g) No ceiling super-jump: holding thrust against a low static ceiling must
  // not bank hidden upward speed. Burn 3 s under a ceiling (the mover clamps the
  // rise), then remove the ceiling with thrust CUT: only retained velocity can
  // lift the body. The old code stored the collision-blocked request in
  // integration_velocity, so ~3 s of net thrust (~20 m/s) launched it metres up.
  {
    Scene s;
    if (!s.Init()) return Fail("(g) init");
    s.Step(20);  // settle on the ground
    // Ceiling bottom at y = 2.8 (box centre 3.0, half 0.2). The 1.8 m capsule's
    // head hits it with the feet ~1.0 m up.
    const physics::BodyId ceiling = s.physics.AddStaticBox({0, 3.0f, 0}, {200, 0.2f, 200});
    for (int i = 0; i < 60 * 3; ++i) {
      s.jin().enabled = true;
      s.jin().thrust = true;
      s.Step(1);
    }
    const f32 blocked_y = s.feet_y();
    // Remove the ceiling and cut thrust; gravity should win almost immediately.
    s.physics.RemoveBody(ceiling);
    s.jin().enabled = false;
    s.jin().thrust = false;
    f32 peak_after = blocked_y;
    f32 vmax_after = 0;
    for (int i = 0; i < 60 * 2; ++i) {
      s.Step(1);
      peak_after = std::max(peak_after, s.feet_y());
      vmax_after = std::max(vmax_after, s.state().velocity.y);
    }
    std::fprintf(stderr,
                 "(g) ceiling: blocked_y=%.2f post-removal peak=%.2f (gain %.2f) vmax_up=%.2f\n",
                 blocked_y, peak_after, peak_after - blocked_y, vmax_after);
    if (blocked_y < 0.5f) return Fail("(g) thrust did not press the character up under the ceiling");
    // One tick of thrust adds only ~0.12 m/s; a super-jump would be ~20 m/s and
    // several metres. Bounds well between the two.
    if (peak_after - blocked_y > 1.0f) return Fail("(g) super-jump: launched after ceiling cleared");
    if (vmax_after > 3.0f) return Fail("(g) super-jump: retained upward velocity too high");
  }

  std::fprintf(stderr, "jetpack_test: all checks passed\n");
  return 0;
}
