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

  void set_water_height(WaterHeightFn fn) { water_height_ = std::move(fn); }

  // Static colliders.
  BodyId AddStaticBox(const Vec3& position, const Vec3& half_extent);
  BodyId AddStaticMesh(const asset::Mesh& mesh, const Vec3& position, const f32 rotation[4],
                       f32 scale);
  // Shared-shape path for streamed instances: the mesh bakes once per key,
  // every placement reuses it through a scale wrapper.
  bool RegisterMeshShape(u64 key, const asset::Mesh& mesh);
  bool has_mesh_shape(u64 key) const;
  BodyId AddStaticMeshInstance(u64 key, const Vec3& position, const f32 rotation[4], f32 scale);
  // Heightfield grid of sample*sample values covering size x size meters,
  // anchored at `origin` (min corner). For streamed terrain cells.
  BodyId AddHeightField(const Vec3& origin, const f32* heights, u32 samples, f32 size);

  // Generic shape-tree bodies (authored collision from the havok decoder or
  // other producers). `scale` converts the desc's units into meters (pass
  // the content's unit->metre scale, e.g. a game-unit scale). Dynamic bodies
  // take an explicit
  // mass in kg; 0 falls back to Jolt's density-derived mass.
  BodyId AddStaticShape(const ShapeDesc& desc, const Vec3& position, const f32 rotation[4],
                        f32 scale);
  // filter_group/subgroup: bodies sharing a filter group collide unless the
  // pair of subgroups was disabled - how a ragdoll's jointed limbs overlap
  // at the hips/shoulders without fighting their constraints.
  BodyId AddDynamicShape(const ShapeDesc& desc, const Vec3& position, const f32 rotation[4],
                         f32 scale, f32 mass, f32 friction, f32 restitution,
                         i32 filter_group = -1, u32 subgroup = 0);
  i32 CreateBodyFilterGroup(u32 subgroup_count);
  void DisableFilterPair(i32 group, u32 sub_a, u32 sub_b);

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
  // Mass in kg. 0 for static or kinematic bodies (infinite mass) and for an
  // invalid handle.
  f32 GetBodyMass(BodyId id) const;
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
  // Driver input, each -1..1 (forward: throttle vs reverse; right: steer) or
  // 0..1 (brake, handbrake). Wakes the body while any input is non-zero.
  void DriveVehicle(VehicleId id, f32 forward, f32 right, f32 brake, f32 handbrake);
  bool GetVehicleTransform(VehicleId id, Vec3* position, f32 rotation[4]) const;
  // World transform of a wheel (cars 0..3 = FL FR RL RR, bikes 0..1 =
  // front/rear), suspension and steering applied.
  bool GetVehicleWheel(VehicleId id, u32 wheel, Vec3* position, f32 rotation[4]) const;
  // Signed speed along the chassis forward axis, m/s.
  f32 VehicleForwardSpeed(VehicleId id) const;

  // Drivetrain + per-wheel telemetry for HUDs, audio and effects.
  struct VehicleState {
    f32 rpm = 0;
    i32 gear = 0;  // Jolt convention: -1 reverse, 0 neutral, 1..N forward
    f32 forward_speed = 0;  // m/s, signed
    u32 wheel_count = 0;
    struct WheelState {
      bool contact = false;
      f32 suspension_length = 0;   // current, meters
      f32 longitudinal_slip = 0;   // 0 = full traction, 1 = locked/spinning
      f32 angular_velocity = 0;    // rad/s around the axle
      f32 rotation_angle = 0;      // accumulated spin, radians
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
  struct Impl;
  std::unique_ptr<Impl> impl_;
  WaterHeightFn water_height_;
  u32 dynamic_count_ = 0;
};

}  // namespace rx::physics

#endif  // RX_PHYSICS_PHYSICS_WORLD_H_
