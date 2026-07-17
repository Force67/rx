#ifndef RX_PHYSICS_PHYSICS_WORLD_H_
#define RX_PHYSICS_PHYSICS_WORLD_H_

#include <functional>
#include <memory>

#include "asset/mesh.h"
#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "physics/cloth.h"
#include "physics/shape_desc.h"

namespace rx::physics {

// Opaque body handle; 0 is invalid.
using BodyId = u64;
// Opaque character-controller handle; 0 is invalid.
using CharacterId = u64;
// Opaque joint/constraint handle; 0 is invalid.
using JointId = u64;
// Opaque wheeled-vehicle handle; 0 is invalid.
using VehicleId = u64;
// Opaque strand-groom handle; 0 is invalid.
using StrandGroomId = u64;

// Ground surface a static collider presents to tires. Drives per-surface tire
// grip (the coupling table in physics_world.cc) and gives audio/FX a surface
// cue per wheel. Asphalt is the default so untagged world geometry keeps full
// street grip and the legacy behaviour. kCount is a sentinel, not a surface.
enum class SurfaceType : u8 {
  kAsphalt,
  kConcrete,
  kDirt,
  kGravel,
  kGrass,
  kSand,
  kSnow,
  kIce,
  kMud,
  kWood,
  kMetal,
  kCount,
};

// Jolt-backed rigid body world. Fixed-step simulation driven from the sim
// stage; dynamic bodies report their transforms back for ECS sync. Bodies
// below a water surface get buoyancy and drag impulses each step (the Jolt
// boat/water sample scheme), with the surface height supplied per position
// so streamed worlds and flat demo sheets share one path.
class RX_PHYSICS_EXPORT PhysicsWorld {
 public:
  // Returns true with the surface height and flow velocity when `position`
  // is over water. Flow drags floating bodies (rivers carry them).
  using WaterHeightFn = std::function<bool(const Vec3& position, f32* height, Vec3* flow)>;

  PhysicsWorld();
  ~PhysicsWorld();

  PhysicsWorld(const PhysicsWorld&) = delete;
  PhysicsWorld& operator=(const PhysicsWorld&) = delete;

  bool Initialize();
  void Update(f32 dt);

  // Installs the water-height callback. THREADING: the callback is invoked only
  // on the game thread - inside Update (before the Jolt step) and from the
  // SampleWater entry below - never from a Jolt worker thread. The vehicle
  // tire-friction path samples each wheel's water into a per-vehicle cache on
  // the game thread and the in-step friction callback reads that cache, so a
  // callback touching thread-affine terrain data or physics queries is safe.
  void set_water_height(WaterHeightFn fn) { water_height_ = std::move(fn); }

  // Global uniform wind velocity (m/s, world space) the force-based aero
  // simulators sample as their ambient airmass. Default zero (still air). Cars
  // ignore it (negligible at this fidelity); the aircraft treats it as the
  // airmass its airspeed/aero are measured against and the boat as a push on
  // its exposed topsides. This is a plain global, not a spatial field.
  void set_wind(const Vec3& wind) { wind_ = wind; }
  Vec3 wind() const { return wind_; }
  // Evaluates the installed water-height callback at `position`. Returns true
  // with the surface height (world Y) and horizontal flow when the point is
  // over water, false when no callback is installed or the point is not over
  // water. The public entry the force-based simulators (tire aquaplaning here,
  // boats/aircraft in later waves) sample the swell through.
  bool SampleWater(const Vec3& position, f32* out_height, Vec3* out_flow) const;

  // Excludes a dynamic body from the generic whole-body buoyancy+drag applied
  // in Update (the Jolt water sample scheme above). A force-based hull that
  // does its own multi-point buoyancy (the boat simulator) opts out here so the
  // two schemes don't stack. No effect when true is set twice; false restores
  // the generic path. Off by default, so ordinary floaters are unchanged.
  void set_buoyancy_exempt(BodyId id, bool exempt);

  // --- rigid-body force primitives (thin Jolt wrappers) ---
  // Continuous force (N) / torque (Nm) accumulated for the next step and
  // cleared by Jolt after it; call every step while the force applies. Point
  // variants take a WORLD-space application point. These wake the body. The
  // building blocks the later force-based boat/aircraft simulators integrate
  // their hydro/aero models on top of.
  void AddForce(BodyId id, const Vec3& force);
  void AddForceAtPoint(BodyId id, const Vec3& force, const Vec3& world_point);
  void AddTorque(BodyId id, const Vec3& torque);
  Vec3 GetLinearVelocity(BodyId id) const;   // m/s, world space
  Vec3 GetAngularVelocity(BodyId id) const;  // rad/s, world space
  // World-space velocity of the world-space point `world_point` rigidly
  // attached to the body (linear + angular contribution).
  Vec3 GetPointVelocity(BodyId id, const Vec3& world_point) const;
  // Mass in kg; 0 for a static/kinematic body or a dead handle.
  f32 GetBodyMass(BodyId id) const;
  // Overrides a dynamic body's inertia tensor with an explicit diagonal
  // (kg*m^2) about the centre of mass, its principal axes aligned to the body
  // axes (no rotation). Replaces the collision-shape-derived tensor, so a
  // force-based simulator whose collision box does not represent the true mass
  // distribution (e.g. an aircraft fuselage box that excludes the wings) gets
  // an honest roll/pitch/yaw response instead of a box-derived one. Components
  // <= 0 leave that axis free (inverse inertia 0 = no angular response about
  // it); pass positive values. No-op on a static/kinematic body or dead handle.
  void SetBodyInertia(BodyId id, const Vec3& diagonal_kgm2);
  // Rescales a dynamic body's mass to `kg` at runtime, scaling its inertia
  // tensor by the same factor so the shape's mass DISTRIBUTION (its principal
  // moments about the centre of mass) is preserved - the moving counterpart to
  // spawning a box at a given density. A force-based simulator whose payload
  // changes (the boat taking on cargo) makes its hull heavier this way instead
  // of respawning it: the extra mass makes it accelerate and turn more slowly
  // (mass AND inertia grow) and, for a buoyant hull, settle deeper. Mirrors
  // SetBodyInertia's inverse-quantity conventions (inertia scales as old/new
  // mass). kg <= 0, a static/kinematic body or a dead handle are ignored.
  void SetBodyMass(BodyId id, f32 kg);

  // Static colliders. The trailing `surface` tags the collider so tire grip
  // and surface FX know what a wheel touches; it defaults to asphalt so
  // existing callers and the physics feel are unchanged (asphalt = full grip,
  // no material installed on the shape).
  BodyId AddStaticBox(const Vec3& position, const Vec3& half_extent,
                      SurfaceType surface = SurfaceType::kAsphalt);
  BodyId AddStaticMesh(const asset::Mesh& mesh, const Vec3& position, const f32 rotation[4],
                       f32 scale, SurfaceType surface = SurfaceType::kAsphalt);
  // Shared-shape path for streamed instances: the mesh bakes once per key,
  // every placement reuses it through a scale wrapper. Because the shape (and
  // so its material) is shared, `surface` is recorded per body on the side and
  // resolved at tire contact, not baked into the shared shape.
  bool RegisterMeshShape(u64 key, const asset::Mesh& mesh);
  bool has_mesh_shape(u64 key) const;
  BodyId AddStaticMeshInstance(u64 key, const Vec3& position, const f32 rotation[4], f32 scale,
                               SurfaceType surface = SurfaceType::kAsphalt);
  // Heightfield grid of sample*sample values covering size x size meters,
  // anchored at `origin` (min corner). For streamed terrain cells.
  BodyId AddHeightField(const Vec3& origin, const f32* heights, u32 samples, f32 size,
                        SurfaceType surface = SurfaceType::kAsphalt);
  // Heightfield with a mixed surface: `material_indices` is (samples-1)^2
  // per-quad indices into `palette` (row-major, matching Jolt's height-field
  // quad layout), so one terrain cell can carry an asphalt road over grass.
  // Falls back to the single-surface path when the palette is empty.
  BodyId AddHeightField(const Vec3& origin, const f32* heights, u32 samples, f32 size,
                        const u8* material_indices, const SurfaceType* palette, u32 palette_count);

  // Generic shape-tree bodies (authored collision from the havok decoder or
  // other producers). `scale` converts the desc's units into meters (pass
  // the content's unit->metre scale, e.g. a game-unit scale). Dynamic bodies
  // take an explicit
  // mass in kg; 0 falls back to Jolt's density-derived mass.
  BodyId AddStaticShape(const ShapeDesc& desc, const Vec3& position, const f32 rotation[4],
                        f32 scale, SurfaceType surface = SurfaceType::kAsphalt);
  // filter_group/subgroup: bodies sharing a filter group collide unless the
  // pair of subgroups was disabled - how a ragdoll's jointed limbs overlap
  // at the hips/shoulders without fighting their constraints.
  BodyId AddDynamicShape(const ShapeDesc& desc, const Vec3& position, const f32 rotation[4],
                         f32 scale, f32 mass, f32 friction, f32 restitution,
                         i32 filter_group = -1, u32 subgroup = 0);
  i32 CreateBodyFilterGroup(u32 subgroup_count);
  void DisableFilterPair(i32 group, u32 sub_a, u32 sub_b);
  // Releases the group's filter-table reference. The i32 slot is retained so
  // group indices stay stable (CreateBodyFilterGroup keeps handing out later
  // indices), and DisableFilterPair / AddDynamicShape treat a released slot as
  // invalid. Safe to call after the group's bodies are gone: each body's Jolt
  // CollisionGroup holds its own RefConst to the table, so the table survives
  // until the last body referencing it is removed, and this only drops our copy
  // of the reference. A no-op on an invalid or already-released slot.
  void ReleaseBodyFilterGroup(i32 group);

  // Ragdoll joints between two dynamic bodies. Frames are 3x4 row-major
  // (basis rows + origin column) in each body's LOCAL space, column 0 = the
  // twist/hinge axis, column 1 = the plane/normal axis, in desc units
  // (scaled by `scale` like the shapes). Angles in radians. The swing-twist
  // joint approximates Havok's asymmetric plane limit with a symmetric one.
  // Return a JointId handle (0 on failure) that the motor/pose API below
  // addresses; also usable as a bool for the plain "did it stick" check.
  JointId AddSwingTwistJoint(BodyId a, BodyId b, const f32 frame_a[12], const f32 frame_b[12],
                             f32 scale, f32 twist_min, f32 twist_max, f32 cone_max,
                             f32 plane_min, f32 plane_max);
  JointId AddHingeJoint(BodyId a, BodyId b, const f32 frame_a[12], const f32 frame_b[12], f32 scale,
                        f32 angle_min, f32 angle_max);

  // Powered-ragdoll motor drive (the "physical hit reaction" primitive). A
  // joint's motor is switched to position mode so the constraint is pulled
  // toward a target relative orientation by a critically-tunable spring
  // (`frequency` in Hz, `damping` ~1.0 for no overshoot). For swing-twist
  // joints both the swing and twist motors are driven; for hinges the single
  // hinge motor. Off by default, so unpowered ragdolls (plain drops) are
  // unaffected.
  void EnableJointMotors(JointId joint, f32 frequency, f32 damping);
  // Sets the motor target as a CONSTRAINT-SPACE orientation quaternion
  // (x,y,z,w): the rotation q that takes body A's constraint frame to body
  // B's, i.e. the pose the drive holds. For swing-twist this is passed to
  // Jolt's SetTargetOrientationCS; for hinges the twist angle about the hinge
  // axis is extracted from the quaternion and used as the target angle. Feed
  // GetJointOrientation()'s spawn-time reading here to hold the bind pose.
  void SetJointMotorTarget(JointId joint, const f32 target_quat[4]);
  // Reads a joint's CURRENT relative orientation in constraint space as a
  // quaternion (x,y,z,w); hinges report a pure rotation about the hinge axis
  // by their current angle. Used to snapshot the bind-pose target and to
  // measure motor tracking error. False on an invalid handle.
  bool GetJointOrientation(JointId joint, f32 out_quat[4]) const;
  // Caps the torque a joint's position motors may apply (N*m); the min limit is
  // set to -max_torque so the motor pushes and pulls symmetrically. Applies to
  // both the swing and twist motors of a swing-twist joint and to the single
  // motor of a hinge. Only meaningful once EnableJointMotors has switched the
  // motors on; a no-op on an invalid handle.
  void SetJointMotorTorqueLimit(JointId joint, f32 max_torque);
  // Switches a joint's motors back off (EMotorState::Off), returning it to the
  // unpowered-ragdoll state so gravity/contacts move it freely. Both swing and
  // twist motors for a swing-twist joint, the single motor for a hinge. A no-op
  // on an invalid handle.
  void DisableJointMotors(JointId joint);
  // Removes a joint: unregisters its constraint from the PhysicsSystem and
  // clears the entry, after which every joint API above no-ops (or returns
  // false) on the stale handle. The JointId slot is retained (handles are index
  // based and must stay stable), so other joints keep their handles.
  //
  // LIFETIME: a live constraint holds raw pointers to its two bodies, which the
  // next Update dereferences (TwoBodyConstraint::IsActive reads mBody1/mBody2),
  // so a body may be removed only AFTER the joints referencing it are gone.
  // Call RemoveJoint on every joint touching a body before RemoveBody on that
  // body. A no-op on an invalid or already-removed handle.
  void RemoveJoint(JointId joint);

  // Applies an instantaneous impulse (kg*m/s) at a body's centre of mass and
  // wakes it. Drives the "get hit" disturbance for the powered-ragdoll test.
  void ApplyImpulse(BodyId id, const Vec3& impulse);
  // Switches a body to kinematic (infinite mass, immovable, immune to
  // gravity/forces) while keeping its layer and collision group. Used to pin
  // a ragdoll's root so the figure hangs from it like a puppet.
  void SetBodyKinematic(BodyId id);

  // --- feedback-controller adapter surface ---
  // These read back and drive individual bodies for a physics-first locomotion
  // controller that closes a loop around measured body state.

  // World-space linear (m/s) and angular (rad/s) velocity of a body. Either
  // output pointer may be null. False for a dead handle (a body that was never
  // added or has been removed), leaving the outputs untouched.
  bool GetBodyVelocity(BodyId id, Vec3* linear, Vec3* angular) const;
  // World-space centre of mass (not the body origin). False for a dead handle,
  // leaving `out` untouched.
  bool GetBodyCenterOfMass(BodyId id, Vec3* out) const;
  // Accumulates a world-space force (N) at the centre of mass, applied over the
  // next Update step; wakes the body. Forces do not persist across steps, so a
  // feedback controller re-applies each tick. A no-op on an invalid handle.
  void ApplyForce(BodyId id, const Vec3& force);
  // Accumulates a world-space torque (N*m) for the next Update step; wakes the
  // body. Like ApplyForce it is consumed by the step, not retained. A no-op on
  // an invalid handle.
  void ApplyTorque(BodyId id, const Vec3& torque);
  // World gravity vector (m/s^2); {0, -9.81, 0} before Initialize.
  Vec3 gravity() const;

  // Dynamic bodies; density in kg/m^3 (wood floats, stone sinks).
  BodyId AddDynamicBox(const Vec3& position, const Vec3& half_extent, f32 density,
                       const Vec3& initial_velocity);
  BodyId AddDynamicSphere(const Vec3& position, f32 radius, f32 density,
                          const Vec3& initial_velocity);

  void RemoveBody(BodyId id);

  // Kinematic capsule: a solid body that never falls or tips and is driven by
  // SetBodyPosition each tick. Used for NPCs and remote players so the local
  // player's character controller collides with them (they block / get shoved)
  // while their authoritative position comes from animation / replication.
  BodyId AddKinematicCapsule(const Vec3& position, f32 radius, f32 half_height);
  // Kinematic box (moving platforms, doors): never falls or tips, pushes
  // characters/bodies out of its way. Drive it with MoveBodyKinematic.
  BodyId AddKinematicBox(const Vec3& position, const Vec3& half_extent);
  // Teleports a body to a new pose (rotation is x,y,z,w). For the kinematic
  // capsules above, called every tick from the entity's transform.
  void SetBodyPosition(BodyId id, const Vec3& position, const f32 rotation[4]);
  // Moves a kinematic body to the target pose over `dt` by assigning its
  // velocity (Jolt MoveKinematic) instead of teleporting, so a character
  // standing on it reads a real ground velocity and rides it. dt <= 0 falls
  // back to SetBodyPosition.
  void MoveBodyKinematic(BodyId id, const Vec3& position, const f32 rotation[4], f32 dt);

  // Kinematic character controller (Jolt CharacterVirtual): a capsule that
  // walks slopes/stairs. `position` is the capsule centre. 0 is invalid.
  CharacterId CreateCharacter(const Vec3& position, f32 radius, f32 half_height);
  // Steps the controller: `horizontal_velocity` is the desired ground-plane
  // velocity (m/s); gravity and jumping are handled internally. Returns the new
  // capsule-centre position and whether it is on the ground.
  void MoveCharacter(CharacterId id, const Vec3& horizontal_velocity, bool jump, f32 dt,
                     Vec3* out_position, bool* out_grounded);
  // Steps the controller with a fully game-owned velocity vector: no internal
  // gravity or jump impulse, so games that integrate their own arcs
  // (platformer variable-height jumps, dashes) keep authority over the
  // vertical axis. `out_ground_velocity` reports the velocity of whatever the
  // character stands on (kinematic movers included) so the caller can ride
  // moving platforms; zero when airborne.
  void MoveCharacterVelocity(CharacterId id, const Vec3& velocity, f32 dt, Vec3* out_position,
                             bool* out_grounded, Vec3* out_ground_velocity = nullptr);
  void SetCharacterPosition(CharacterId id, const Vec3& position);
  // Capsule-centre position of a live controller. False for a dead handle.
  bool GetCharacterPosition(CharacterId id, Vec3* out_position) const;
  // Stair/slope locomotion tuning honoured by the Move* calls above.
  // `max_slope_angle` (radians) is the steepest ground the controller treats as
  // walkable (steeper contacts push it back instead of supporting it);
  // `step_height` (metres) is the tallest ledge the stair-walking pass lifts
  // over. Defaults match Jolt's stock controller (~50 deg, 0.4 m).
  void ConfigureCharacter(CharacterId id, f32 max_slope_angle, f32 step_height);
  // Swaps the controller's capsule to new dimensions in place. Returns false
  // (leaving the old shape) when the new capsule would start out
  // interpenetrating world geometry by more than a small margin, so a game can
  // keep a taller stance until there is headroom. The capsule stays centred on
  // the same position; callers that want feet-planted resizing adjust the
  // position first.
  bool SetCharacterShape(CharacterId id, f32 radius, f32 half_height);

  // Wheeled vehicle (Jolt VehicleConstraint + WheeledVehicleController): a
  // dynamic chassis box with four suspension wheels and an automatic
  // transmission. Engine space, +Z forward, +Y up; wheel order is FL, FR,
  // RL, RR (front wheels steer, rear wheels take the handbrake). The caller
  // renders the chassis/wheels from the transforms below; there is no wheel
  // geometry in the physics world (wheels are suspension raycasts). The
  // racing-sim fields all default to "keep the legacy arcade tune" (0 or -1
  // = use the previous hardcoded/Jolt default), so existing consumers are
  // unchanged.
  enum class Drivetrain : u8 { kRWD, kFWD, kAWD };
  struct VehicleDesc {
    Vec3 half_extent{0.9f, 0.5f, 2.0f};  // chassis box half extents
    f32 mass = 1500;                     // kg
    f32 wheel_radius = 0.34f;
    f32 wheel_width = 0.25f;
    f32 wheel_x = 0.85f;   // half track width (wheel center from chassis center)
    f32 front_z = 1.4f;    // front axle offset, +Z = forward
    f32 rear_z = -1.3f;
    f32 suspension_min = 0.15f;  // suspension travel below the wheel attach
    f32 suspension_max = 0.4f;
    f32 max_engine_torque = 600;
    f32 max_steer_angle = 0.6f;  // radians
    // Center of mass dropped below the chassis center: arcade stability (hard
    // to roll in normal cornering, still flippable off ramps).
    f32 com_drop = 0.4f;
    // Center of mass shifted along the body +Z (forward) axis, metres; negative
    // = rearward. 0 = centred (legacy). A rearward CoM (loaded van/truck) puts
    // weight over the rear axle, lightens the steer and drops the nose under
    // braking; used by the van's cargo-load parameter. Mapped straight onto the
    // chassis OffsetCenterOfMass Z with com_drop's Y.
    f32 com_fore = 0;

    // --- racing-sim extensions ---
    Drivetrain drivetrain = Drivetrain::kRWD;
    f32 awd_front_split = 0.4f;  // kAWD: engine torque fraction to the front axle
    // Gearbox: gear_count 0 keeps Jolt's default 5-speed. Ratios are
    // engine:gearbox; final_drive is the differential ratio (0 = default).
    u32 gear_count = 0;
    f32 gear_ratios[8] = {};
    f32 final_drive = 0;
    f32 shift_up_rpm = 0;   // 0 = default (4000)
    f32 shift_down_rpm = 0; // 0 = default (2000)
    f32 max_rpm = 0;        // 0 = default (6000)
    f32 min_rpm = 0;        // 0 = default (1000)
    // Engine flywheel inertia, kg m^2 (0 = Jolt default 0.5). Through a big
    // total reduction this reflects as ratio^2 * inertia of extra vehicle
    // mass, so light vehicles want a realistic (small) value.
    f32 engine_inertia = 0;
    f32 clutch_strength = 0;  // 0 = legacy (10)
    // Engine braking: angular damping of the engine (dw/dt = -c*w), the drag
    // felt off-throttle. 0 = Jolt default (0.2); raise for a diesel-like
    // trailing-throttle deceleration. max_rpm above doubles as the rev limiter
    // (Jolt hard-clamps engine RPM to it).
    f32 engine_braking = 0;
    // Normalized engine torque curve: `torque_curve[i]` = (rpm fraction 0..1 of
    // max_rpm, torque fraction 0..1 of max_engine_torque), ascending in x.
    // Mapped onto Jolt's VehicleEngineSettings normalized torque curve. Leave
    // torque_curve_count 0 to keep Jolt's stock curve (0.8 / 1.0 / 0.8); a
    // count above the array size is clamped to it at use.
    struct TorquePoint {
      f32 rpm_fraction = 0;
      f32 torque_fraction = 0;
    };
    TorquePoint torque_curve[8] = {};
    u32 torque_curve_count = 0;
    // Tire grip: scales Jolt's default longitudinal/lateral slip curves
    // (1 = stock street tire, ~1.5+ slicks). 0 = default (1).
    f32 tire_long_friction = 0;
    f32 tire_lat_friction = 0;
    // Suspension spring per wheel; 0 = Jolt default (1.5 Hz / 0.5).
    f32 suspension_frequency = 0;
    f32 suspension_damping = 0;
    f32 anti_roll_stiffness = 0;    // 0 = Jolt default (1000)
    f32 max_brake_torque = 0;       // Nm per wheel; 0 = Jolt default (1500)
    f32 max_handbrake_torque = -1;  // rear axle; < 0 = legacy (8000)
    // Aero downforce applied at the COM along the body -Y each step:
    // F = downforce * forward_speed^2 (N). 0 = off.
    f32 downforce = 0;
    // Driver aid: cuts throttle when driven-wheel longitudinal slip exceeds
    // ~8%, holding the tire near its grip peak (and letting the automatic
    // box shift, which Jolt gates on slip). Off = raw throttle.
    bool traction_control = false;
    // --- handling-profile extensions (all default to current behaviour) ---
    // Limited-slip differential: Jolt's mLimitedSlipRatio (max/min driven-wheel
    // speed before all torque routes to the slower wheel). Lower = tighter lock
    // (a spinning inside wheel still drives the car out of a corner, adds
    // throttle-on oversteer); large (>=100) approaches an open diff. 0 = Jolt
    // default (1.4). Applied to every driven differential.
    f32 limited_slip_ratio = 0;
    // High-speed steering fade: the effective steer command scales from 1 at
    // rest down to steer_high_speed_fraction as forward speed reaches
    // steer_fade_speed (m/s), then holds. Models the rack calming down at speed
    // so a full flick doesn't spin the car on the motorway. fraction >= 1 or
    // fade_speed <= 0 = no fade (legacy full-angle steering at any speed).
    f32 steer_high_speed_fraction = 1.0f;
    f32 steer_fade_speed = 0;
    // Anti-roll bar stiffness per axle, N/m (Jolt VehicleAntiRollBar). 0 on an
    // axle falls back to anti_roll_stiffness, then to Jolt's default (1000).
    // More front bar than rear pushes the balance toward understeer, more rear
    // bar toward oversteer.
    f32 anti_roll_front = 0;
    f32 anti_roll_rear = 0;
    // Brake bias: fraction of the per-wheel brake torque sent to the FRONT axle
    // (0.5 = even, road cars ~0.6). max_brake_torque stays the per-wheel base;
    // each front wheel receives 2*bias of it and each rear 2*(1-bias). Left at
    // 0.5 with max_brake_torque 0, the Jolt default even braking is unchanged.
    f32 brake_bias_front = 0.5f;
    // Aero downforce balance: fraction of `downforce` pressed at the FRONT axle
    // (the rest at the rear), each applied at its axle instead of the CoM. The
    // split loads the axles asymmetrically (front-biased downforce grows front
    // grip) and adds a slight aero pitch. 0.5 keeps the legacy single CoM force.
    f32 downforce_balance = 0.5f;
    // Per-axle lateral tyre grip scalars (like tire_lat_friction, but per axle):
    // 0 on an axle falls back to tire_lat_friction, then to 1 (stock). Front
    // below rear = understeer (the nose washes wide); rear below front =
    // throttle-on oversteer (the tail steps out). The key knob behind the
    // muscle car's tail-happiness and the hatchback's safe understeer.
    f32 front_lat_friction = 0;
    f32 rear_lat_friction = 0;
    // Per-axle suspension spring frequency, Hz: 0 on an axle falls back to
    // suspension_frequency, then to Jolt's default (1.5). A softer rear lets a
    // muscle car squat and hook up; soft on both axles gives a wallowy SUV/van.
    f32 front_suspension_frequency = 0;
    f32 rear_suspension_frequency = 0;
    // Free-rolling (unpowered) chassis: no engine torque reaches any wheel, so
    // all four wheels roll purely on their suspension and tire friction, the
    // way a trailer or a horse-drawn carriage pulled through an external hitch
    // does. The front axle still steers from DriveVehicle's steer input (a
    // turntable front axle) and the rear axle still takes the handbrake as a
    // parking brake. Off by default (an ordinary engine-driven car).
    bool free_rolling = false;
  };
  // Spawns the chassis at `position` (chassis center) yawed around +Y. Spawn
  // slightly high and let the suspension settle. 0 on failure.
  VehicleId CreateVehicle(const VehicleDesc& desc, const Vec3& position, f32 yaw_radians);

  // Motorcycle (Jolt MotorcycleController): two-wheeler with a lean spring
  // that banks the bike into corners and a speed-aware steering limit so it
  // doesn't topple. Wheel 0 = front (steers, caster-raked fork), 1 = rear
  // (drive). Same handle space and Drive/Get API as cars; handbrake input is
  // ignored.
  struct MotorcycleDesc {
    Vec3 half_extent{0.2f, 0.3f, 0.9f};  // chassis box half extents
    f32 mass = 240;                      // kg, bike + rider
    f32 com_drop = 0.3f;                 // rider crouch keeps it low
    f32 wheel_radius = 0.31f;
    f32 wheel_width = 0.11f;
    f32 front_z = 0.75f;  // axle offsets, +Z = forward
    f32 rear_z = -0.75f;
    f32 caster_angle = 0.52f;  // front fork rake from vertical, radians
    f32 suspension_min = 0.15f;
    f32 suspension_max = 0.35f;
    f32 front_suspension_frequency = 1.5f;  // Hz
    f32 rear_suspension_frequency = 2.0f;
    f32 max_steer_angle = 0.52f;  // radians; the lean limiter shrinks it with speed
    f32 max_engine_torque = 150;
    f32 max_rpm = 10000;
    f32 min_rpm = 1000;
    // Bike engines are light: Jolt's default 0.5 kg m^2 flywheel through a
    // ~13:1 reduction reflects as ~4x the whole bike's mass and eats the
    // torque as engine spin-up.
    f32 engine_inertia = 0.08f;
    f32 engine_braking = 0;  // engine angular damping; 0 = Jolt default (0.2)
    // Normalized torque curve, like VehicleDesc::torque_curve. 0 = stock curve.
    VehicleDesc::TorquePoint torque_curve[8] = {};
    u32 torque_curve_count = 0;
    u32 gear_count = 0;  // 0 = a stock 6-speed bike box
    f32 gear_ratios[8] = {};
    f32 final_drive = 4.8f;  // primary + sprocket combined
    f32 shift_up_rpm = 8000;
    f32 shift_down_rpm = 2000;
    f32 front_brake_torque = 500;
    f32 rear_brake_torque = 250;
    f32 tire_long_friction = 0;  // like VehicleDesc: 0 = default curves
    f32 tire_lat_friction = 0;
    f32 max_lean_angle = 0.79f;  // radians (~45 deg)
    f32 lean_spring = 5000;
    f32 lean_damping = 1000;
    f32 downforce = 0;
    bool traction_control = false;  // same throttle-governing aid as cars
  };
  VehicleId CreateMotorcycle(const MotorcycleDesc& desc, const Vec3& position, f32 yaw_radians);

  void RemoveVehicle(VehicleId id);
  // Global rain wetness, 0 (dry) .. 1 (soaked), scaling tire grip down per
  // surface (wet asphalt ~0.7 of dry at 1.0; loose dirt turns mud-like). Off
  // (0) by default so dry-road behaviour is unchanged.
  void set_surface_wetness(f32 wetness);
  // Manual transmission: gears change only on the shift_up/shift_down edges of
  // the VehicleInput overload and the clutch input is honoured (auto mode
  // otherwise). Ignored until a vehicle is created; both cars and bikes accept
  // it. Off by default (automatic box, auto clutch), matching the 4-float
  // DriveVehicle below.
  void SetManualTransmission(VehicleId id, bool manual);
  // Driver input, each -1..1 (forward: throttle vs reverse; right: steer) or
  // 0..1 (brake, handbrake). Wakes the body while any input is non-zero.
  // Automatic box + auto clutch.
  void DriveVehicle(VehicleId id, f32 forward, f32 right, f32 brake, f32 handbrake);
  // Full driver input. throttle/steer are -1..1, brake/handbrake/clutch are
  // 0..1 (clutch: 1 = fully disengaged). shift_up/shift_down are edges honoured
  // only in manual mode (SetManualTransmission), where a rising edge changes one
  // gear; they are ignored by the automatic box. In automatic mode clutch is
  // ignored (the box works the clutch itself).
  struct VehicleInput {
    f32 throttle = 0;    // -1..1, forward vs reverse gas
    f32 steer = 0;       // -1..1, right positive
    f32 brake = 0;       // 0..1
    f32 handbrake = 0;   // 0..1 (rear axle; ignored by bikes)
    f32 clutch = 0;      // 0..1, 1 = fully disengaged (manual only)
    bool shift_up = false;
    bool shift_down = false;
  };
  void DriveVehicle(VehicleId id, const VehicleInput& input);
  bool GetVehicleTransform(VehicleId id, Vec3* position, f32 rotation[4]) const;
  // World transform of a wheel (cars 0..3 = FL FR RL RR, bikes 0..1 =
  // front/rear), suspension and steering applied.
  bool GetVehicleWheel(VehicleId id, u32 wheel, Vec3* position, f32 rotation[4]) const;
  // Signed speed along the chassis forward axis, m/s.
  f32 VehicleForwardSpeed(VehicleId id) const;
  // The dynamic chassis body backing a vehicle, as a BodyId usable with the
  // rigid-body force primitives above (AddForceAtPoint / GetPointVelocity /
  // GetBodyMass). Lets a caller stage external forces on the chassis, e.g. a
  // tow hitch pulling a free-rolling carriage toward the horse. 0 for a dead
  // handle.
  BodyId GetVehicleBody(VehicleId id) const;

  // Drivetrain + per-wheel telemetry for HUDs, audio and effects.
  struct VehicleState {
    f32 rpm = 0;
    i32 gear = 0;  // Jolt convention: -1 reverse, 0 neutral, 1..N forward
    f32 forward_speed = 0;  // m/s, signed
    // Engine load 0..1: actual delivered torque / max torque available at the
    // current rpm (the throttle the box is actually letting through, after the
    // clutch). engine_torque is that delivered torque in Nm. is_shifting is
    // true while the clutch is slipping through a gear change.
    f32 engine_load = 0;
    f32 engine_torque = 0;  // Nm
    bool is_shifting = false;
    u32 wheel_count = 0;
    struct WheelState {
      bool contact = false;
      f32 suspension_length = 0;   // current, meters
      f32 suspension_compression = 0;  // 0 = fully extended, 1 = fully compressed
      f32 longitudinal_slip = 0;   // 0 = full traction, 1 = locked/spinning
      f32 lateral_slip = 0;        // radians: slip angle between ground and wheel
      f32 angular_velocity = 0;    // rad/s around the axle
      f32 rotation_angle = 0;      // accumulated spin, radians
      SurfaceType surface = SurfaceType::kAsphalt;  // ground in contact
      bool submerged = false;      // contact point below the water surface
      f32 wading_depth = 0;        // meters of water over the contact patch
    } wheels[4];
  };
  bool GetVehicleState(VehicleId id, VehicleState* out) const;

  // Strand groom (Jolt soft-body Cosserat rods): hair guide strands simulated
  // on the CPU inside the physics step. Nodes are strand-major
  // (strand_count * points_per_strand xyz triplets) in a groom-local frame;
  // node 0 of each strand is the root, pinned to the groom transform (the
  // animated head bone). Hairstyles are data, not code: `pins` fix extra
  // mid-strand nodes in the groom frame (a ponytail tie, bun pinning),
  // `binds` weave nodes of different strands together (braids, dreadlocks),
  // and the compliance/damping scalars set the per-style feel (compliance is
  // inverse stiffness, 0 = rigid).
  struct StrandGroomDesc {
    const f32* points = nullptr;  // strand_count * points_per_strand * 3
    u32 strand_count = 0;
    u32 points_per_strand = 0;
    // (strand, point) pairs pinned to the groom frame beyond the roots.
    const u32* pins = nullptr;
    u32 pin_count = 0;
    // (strand_a, point_a, strand_b, point_b) cross-strand ties, kept at their
    // rest-pose distance with `bind_compliance`.
    const u32* binds = nullptr;
    u32 bind_count = 0;
    // Coarse collision proxy for the owning character (scalp/shoulders) in
    // the groom frame; follows the groom transform as one kinematic body.
    struct Sphere {
      Vec3 center;
      f32 radius = 0;
    };
    const Sphere* spheres = nullptr;
    u32 sphere_count = 0;
    struct Capsule {
      Vec3 a;  // segment endpoints
      Vec3 b;
      f32 radius = 0;
    };
    const Capsule* capsules = nullptr;
    u32 capsule_count = 0;
    // Per-style feel.
    f32 stretch_compliance = 0;      // rod stretch/shear (0 = inextensible)
    f32 bend_compliance = 0.02f;     // rod bend/twist (higher = looser hair)
    f32 bind_compliance = 1e-5f;     // cross-strand ties
    f32 damping = 0.15f;             // linear velocity damping
    f32 gravity_factor = 1.0f;
    f32 node_mass = 0.02f;           // kg per dynamic node
    f32 node_radius = 0.0015f;       // collision radius around each node
    f32 max_stretch = 1.03f;         // long-range attachment: max node distance
                                     // from its pin as a fraction of rest arc length
    u32 iterations = 5;              // solver iterations per step
  };
  // Spawns the groom with `transform` placing the groom-local frame in the
  // world. 0 on failure.
  StrandGroomId CreateStrandGroom(const StrandGroomDesc& desc, const Mat4& transform);
  // Retargets the pinned nodes (and the collision proxy) to a new groom
  // transform; call each frame with the animated head transform. The pins are
  // pulled to their targets over the next fixed step so the free hair reads a
  // real root velocity; dt <= 0 teleports instead (spawn, cuts).
  void SetStrandGroomTransform(StrandGroomId id, const Mat4& transform, f32 dt);
  // Wind as an acceleration (m/s^2) applied to the whole groom each step; the
  // caller varies it over time for gusts. Zero (the default) applies nothing.
  void SetStrandGroomWind(StrandGroomId id, const Vec3& wind);
  // Number of f32s GetStrandGroomPositions writes (node count * 3); 0 for a
  // dead handle.
  u32 StrandGroomPositionCount(StrandGroomId id) const;
  // World-space node positions, strand-major xyz, for the hair renderer.
  bool GetStrandGroomPositions(StrandGroomId id, f32* out, u32 count) const;
  void RemoveStrandGroom(StrandGroomId id);

  // Arbitrary triangle cloth. Jolt owns structural XPBD constraints, skeletal
  // skinning, pressure and rigid collision; rx adds swept-BVH continuous
  // vertex/triangle and edge/edge self-collision. The same path handles open
  // curtains, cylindrical skirts and consistently wound closed inflatables.
  ClothId CreateCloth(const ClothDesc& desc, const Mat4& transform);
  // Retargets descriptor pins through a new object transform. Returns false
  // for invalid or unpinned cloth. dt <= 0 is an intentional teleport/reset;
  // positive dt preserves attachment velocity.
  bool SetClothTransform(ClothId id, const Mat4& transform, f32 dt);
  // Retargets pins directly in descriptor pin order, for rails/hooks whose
  // motion is not represented by one transform. Targets are world-space. Fast
  // retargets are limited by max_linear_velocity; use dt = 0 to teleport.
  bool SetClothPinTargets(ClothId id, const Vec3* targets, u32 target_count, f32 dt);
  // Updates native Jolt skin targets. `hard_reset` snaps every skinned vertex
  // to animation and is intended for spawn/teleport, not ordinary motion.
  bool SetClothJointTransforms(ClothId id, const Mat4* world_joints, u32 joint_count,
                               bool hard_reset = false);
  // World-space air velocity in m/s. Aerodynamic force depends on triangle
  // area, orientation and relative cloth velocity; zero disables the pass.
  void SetClothWind(ClothId id, const Vec3& velocity);
  // Jolt pressure coefficient (n*R*T), for one outward-wound closed shell.
  void SetClothPressure(ClothId id, f32 pressure);
  u32 ClothVertexCount(ClothId id) const;
  bool GetClothPositions(ClothId id, Vec3* out, u32 count) const;
  void RemoveCloth(ClothId id);

  // Closest hit of a ray; used by foot IK to find the ground under a foot.
  struct RayHit {
    Vec3 position;
    Vec3 normal{0, 1, 0};
    f32 distance = 0;
  };
  bool Raycast(const Vec3& origin, const Vec3& direction, f32 max_distance, RayHit* out) const;
  // Same closest-hit ray, but skipping the body `ignore` (its whole shape).
  // Lets a force-based simulator cast from a point INSIDE its own collision
  // shape without hitting itself - the aircraft's gear suspension rays start
  // at real hardpoints on the fuselage and would otherwise register the plane's
  // own underside. `ignore` == 0 skips nothing (identical to the overload
  // above). Jolt IgnoreSingleBodyFilter.
  bool Raycast(const Vec3& origin, const Vec3& direction, f32 max_distance, RayHit* out,
               BodyId ignore) const;
  // Same query while skipping every body in `ignored`. The list is borrowed for
  // the duration of the call and may contain zero handles. Dynamic platforms not
  // present in the list remain valid hits.
  bool Raycast(const Vec3& origin, const Vec3& direction, f32 max_distance, RayHit* out,
               const BodyId* ignored, u32 ignored_count) const;

  // Closest hit of a swept sphere of `radius` from `origin` along `direction`
  // for up to `max_distance` metres. Used by third-person camera collision and
  // the character controller's uncrouch headroom probe. `out->distance` is the
  // travelled distance to first contact (0 when the sphere already overlaps at
  // the origin); false when nothing is hit.
  bool SphereCast(const Vec3& origin, const Vec3& direction, f32 max_distance, f32 radius,
                  RayHit* out) const;

  // Pose of a (dynamic) body for ECS sync.
  bool GetBodyTransform(BodyId id, Vec3* position, f32 rotation[4]) const;

  // Contact recording for a small set of watched bodies (a locomotion
  // controller watches the feet to detect ground contact and its location).
  // Contacts are recorded by a Jolt contact listener during Update and stay
  // readable until the next Update clears them.
  struct BodyContact {
    Vec3 position;         // world-space contact point (average of the
                           // manifold points on the watched body's surface)
    Vec3 normal{0, 1, 0};  // world-space unit normal pointing INTO the watched
                           // body (Jolt's manifold normal, body1->body2,
                           // flipped as needed): +Y for a foot on the floor
    f32 impulse = 0;       // estimated normal impulse this step (kg*m/s) from
                           // Jolt's EstimateCollisionResponse; ~0 for a settled
                           // resting contact, and 0 if it could not be estimated
    BodyId other = 0;      // the other body's handle (always one of ours, since
                           // every body comes from this API); 0 only if unknown
  };
  // Starts recording contacts for `id`. Idempotent; cheap to call for a handful
  // of bodies (feet). A no-op on an invalid handle.
  void WatchBodyContacts(BodyId id);
  // Stops recording and drops any buffered contacts for `id`. After this,
  // GetBodyContacts returns 0. A no-op on an unwatched or invalid handle.
  void UnwatchBodyContacts(BodyId id);
  // Copies up to `max_contacts` contacts recorded for `id` during the last
  // Update into `out`, returning the number copied. 0 for an unwatched or
  // invalid handle, or when no contact occurred last step.
  u32 GetBodyContacts(BodyId id, BodyContact* out, u32 max_contacts) const;

  u32 dynamic_body_count() const { return dynamic_count_; }
  bool initialized() const { return impl_ != nullptr; }

 private:
  // Installs the surface-aware tire-friction combine callback on a freshly
  // created vehicle (index into impl_->vehicles). Both cars and bikes share it.
  void InstallVehicleFriction(u32 vehicle_index);
  // Traction-control throttle shaping (governs the driven-wheel slip toward the
  // grip peak), shared by the automatic and manual driver-input paths so manual
  // mode gets the same aid. Returns the shaped throttle; a no-op when the
  // vehicle has no traction control or `forward` is zero.
  f32 TractionControlThrottle(u32 vehicle_index, f32 forward);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  WaterHeightFn water_height_;
  Vec3 wind_{};              // global uniform wind velocity, m/s, world space
  f32 surface_wetness_ = 0;  // global rain wetness, 0..1
  u32 dynamic_count_ = 0;
};

}  // namespace rx::physics

#endif  // RX_PHYSICS_PHYSICS_WORLD_H_
