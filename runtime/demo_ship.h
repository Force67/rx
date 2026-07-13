#ifndef RX_RUNTIME_DEMO_SHIP_H_
#define RX_RUNTIME_DEMO_SHIP_H_

#include <base/containers/vector.h>

#include "core/math.h"
#include "engine_context.h"
#include "physics/physics_world.h"
#include "render/core/renderer.h"

namespace rx {

// Procedural sailing-ship demo slice (--demo ship): a lofted wooden brig with
// wind-billowed sails, verlet rope rigging and timed cannon broadsides, cruising
// on the adaptive-water / FFT-ocean stack. Content only - it drives the ship
// through the same public physics/render APIs the water demo already uses
// (AddDynamicBox + the buoyancy callback, WaterDisturbance wakes, additive demo
// particles), touching no engine internals.
class ShipDemo {
 public:
  explicit ShipDemo(EngineContext& ctx);

  // Builds the ocean sheet, both vessels' static geometry and the flagship's
  // dynamic hull body. Call once from DemoScenes::CreateDemoScene.
  void Create();

  // Advances sail flutter, rope verlet, cannon timers, projectiles and
  // particles; emits every ship draw, the hull wake and impact foam into this
  // frame's view. Called from DemoScenes::EmitToView.
  void Emit(f32 dt, render::FrameView& view);

 private:
  // A rigid piece of a vessel: an uploaded mesh drawn at hull_xform * local.
  struct Part {
    u64 mesh = 0;
    Mat4 local = Mat4::Identity();
  };

  // A camera-facing-agnostic verlet rope: fixed endpoints, free interior nodes
  // re-integrated each frame under gravity + wind, all in ship-local space.
  struct Rope {
    base::Vector<Vec3> pos;   // current node positions (local)
    base::Vector<Vec3> prev;  // previous positions (verlet velocity)
    f32 seg_len = 0;          // rest length between adjacent nodes
    f32 radius = 0.05f;       // ribbon half-width
    bool pin_last = true;     // both ends pinned (stays/shrouds) vs free tail
  };

  struct Particle {
    Vec3 pos, vel, color;
    f32 life = 0, max_life = 1, size = 0.1f;
  };

  // A cannonball in flight: a ballistic mesh draw that splashes on the surface.
  struct Projectile {
    Vec3 pos, vel;
    bool alive = false;
  };

  struct MuzzleLight {
    Vec3 pos;
    f32 life = 0;
  };

  // Geometry builders (anonymous-namespace-free so they can share the class's
  // material ids). Each appends into a fresh mesh uploaded by Create.
  void BuildFlagship();
  void BuildAnchoredShip();
  void BuildRopes();

  void UpdateShipMotion(f32 dt, render::FrameView& view);
  void UpdateSailsAndRopes(f32 dt, render::FrameView& view);
  void UpdateCannons(f32 dt, render::FrameView& view);
  void UpdateParticles(f32 dt, render::FrameView& view);
  void EmitWake(render::FrameView& view);
  void FollowCamera();

  f32 Rand();

  EngineContext& ctx_;
  ecs::World& world_;
  render::Renderer& renderer_;
  FlyCamera& camera_;
  physics::PhysicsWorld& physics_;
  const EngineConfig& config_;

  // Flagship hull dynamic body + its cached pose (mirrored from Jolt each frame).
  physics::BodyId hull_body_ = 0;
  Vec3 hull_pos_{0, 0, 0};
  Quat hull_rot_{0, 0, 0, 1};
  Vec3 hull_prev_pos_{0, 0, 0};
  Mat4 hull_xform_ = Mat4::Identity();
  Mat4 hull_prev_xform_ = Mat4::Identity();
  Vec3 hull_vel_{0, 0, 0};

  base::Vector<Part> flagship_parts_;  // hull + rig + sails (drawn at hull pose)
  base::Vector<Part> anchored_parts_;  // second vessel, fixed world pose
  Mat4 anchored_xform_ = Mat4::Identity();
  Mat4 anchored_rig_xform_ = Mat4::Identity();  // static rig curve for the tow

  base::Vector<Rope> ropes_;
  u64 rope_mesh_ = 0;  // per-frame re-uploaded ribbon mesh (see .cc pitfall note)
  bool rope_mesh_live_ = false;

  // Cannon gunports in ship-local space; broadside timer alternates sides.
  base::Vector<Vec3> ports_;      // port muzzle positions (local)
  base::Vector<Vec3> port_dirs_;  // local outward firing directions
  f32 cannon_timer_ = 2.5f;
  int broadside_side_ = 1;  // 0 = +X (port rail), 1 = -X (starboard, toward the chase cam)

  base::Vector<Particle> particles_;
  base::Vector<Projectile> projectiles_;
  base::Vector<MuzzleLight> muzzle_lights_;

  u64 ball_mesh_ = 0;  // shared cannonball sphere
  u32 rng_ = 0x1234abcdu;
  f32 time_ = 0;
  bool ready_ = false;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEMO_SHIP_H_
