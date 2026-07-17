#include "physics/physics_world.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/MotorSettings.h>
#include <Jolt/Physics/Constraints/SpringSettings.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/PhysicsMaterialSimple.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodySharedSettings.h>
#include <Jolt/Physics/Vehicle/MotorcycleController.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>

#include <base/containers/unordered_map.h>

#include "core/feature_registry.h"
#include "core/log.h"
#include "physics/cloth_collision.h"

static_assert(JPH_VERSION_MAJOR > 5 || (JPH_VERSION_MAJOR == 5 && JPH_VERSION_MINOR >= 6),
              "rx cloth and strand simulation require Jolt 5.6 or newer");

namespace rx::physics {
namespace {

namespace layers {
constexpr JPH::ObjectLayer kStatic = 0;
constexpr JPH::ObjectLayer kDynamic = 1;
constexpr u32 kCount = 2;
}  // namespace layers

namespace broad {
constexpr JPH::BroadPhaseLayer kStatic{0};
constexpr JPH::BroadPhaseLayer kDynamic{1};
constexpr u32 kCount = 2;
}  // namespace broad

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return broad::kCount; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return layer == layers::kStatic ? broad::kStatic : broad::kDynamic;
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    return layer == broad::kStatic ? "static" : "dynamic";
  }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_layer) const override {
    // Statics never collide with the static broadphase.
    if (layer == layers::kStatic) return broad_layer == broad::kDynamic;
    return true;
  }
};

class ObjectLayerPair final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    return a == layers::kDynamic || b == layers::kDynamic;
  }
};

void TraceCallback(const char* fmt, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  RX_INFO("jolt: {}", buffer);
}

JPH::Vec3 ToJolt(const Vec3& v) { return {v.x, v.y, v.z}; }

JPH::Mat44 ToJolt(const Mat4& m) {
  return {JPH::Vec4(m.m[0], m.m[1], m.m[2], m.m[3]),
          JPH::Vec4(m.m[4], m.m[5], m.m[6], m.m[7]),
          JPH::Vec4(m.m[8], m.m[9], m.m[10], m.m[11]),
          JPH::Vec4(m.m[12], m.m[13], m.m[14], m.m[15])};
}

bool IsFinite(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool IsFinite(const Mat4& value) {
  for (f32 element : value.m) {
    if (!std::isfinite(element)) return false;
  }
  return true;
}

// Friction of static world geometry (ground, roads, walls). Jolt's default
// of 0.2 is ice: vehicle tires combine as sqrt(tire * body), so the body
// side must carry a real asphalt/concrete-grade value. (For tire contacts the
// surface-grip table below supersedes this; it still governs box/character
// contacts.)
constexpr f32 kStaticFriction = 0.9f;

constexpr u32 kSurfaceCount = static_cast<u32>(SurfaceType::kCount);

// Each SurfaceType is backed by one PhysicsMaterialSimple instance (the palette
// in Impl), installed on tagged colliders. The tire-friction combine callback
// resolves the material back to a SurfaceType by pointer identity against that
// palette (Jolt is built without C++ RTTI, so no dynamic_cast). A plain
// PhysicsMaterialSimple avoids emitting a typeinfo reference Jolt can't satisfy.

// Per-surface tire grip. `longitudinal`/`lateral` scale the tire's own
// friction-curve peak (1 = full street-tire grip on dry asphalt). `wet` is the
// grip multiplier at full rain wetness (lerped from 1 at dry): asphalt loses
// ~30%, loose dirt drops toward mud, ice/snow change little. Documented tune,
// not measured: tune here, not per game.
struct SurfaceGripEntry {
  f32 longitudinal;
  f32 lateral;
  f32 wet;
};
constexpr SurfaceGripEntry kSurfaceGrip[kSurfaceCount] = {
    /* kAsphalt  */ {1.00f, 1.00f, 0.70f},
    /* kConcrete */ {0.98f, 0.98f, 0.75f},
    /* kDirt     */ {0.75f, 0.72f, 0.55f},
    /* kGravel   */ {0.80f, 0.75f, 0.70f},
    /* kGrass    */ {0.65f, 0.60f, 0.55f},
    /* kSand     */ {0.55f, 0.50f, 0.60f},
    /* kSnow     */ {0.45f, 0.40f, 0.80f},
    /* kIce      */ {0.15f, 0.12f, 0.70f},
    /* kMud      */ {0.45f, 0.40f, 0.85f},
    /* kWood     */ {0.85f, 0.82f, 0.65f},
    /* kMetal    */ {0.70f, 0.65f, 0.55f},
};

const char* SurfaceName(SurfaceType s) {
  switch (s) {
    case SurfaceType::kAsphalt: return "asphalt";
    case SurfaceType::kConcrete: return "concrete";
    case SurfaceType::kDirt: return "dirt";
    case SurfaceType::kGravel: return "gravel";
    case SurfaceType::kGrass: return "grass";
    case SurfaceType::kSand: return "sand";
    case SurfaceType::kSnow: return "snow";
    case SurfaceType::kIce: return "ice";
    case SurfaceType::kMud: return "mud";
    case SurfaceType::kWood: return "wood";
    case SurfaceType::kMetal: return "metal";
    case SurfaceType::kCount: break;
  }
  return "asphalt";
}

// Rain wetness (0..1) scaling of a surface's grip, lerped toward its wet value.
f32 WetGripMultiplier(SurfaceType s, f32 wetness) {
  const f32 wet = kSurfaceGrip[static_cast<u32>(s)].wet;
  return 1.0f + (wet - 1.0f) * std::clamp(wetness, 0.0f, 1.0f);
}

// Aquaplaning: grip fades as the contact patch floods (depth relative to the
// wheel) and as speed builds a water wedge the tread can't clear. Returns a
// 0.1..1 grip multiplier. Onset ~8 m/s, full hydroplaning ~25 m/s; a patch is
// "fully awash" at half the wheel radius of standing water. Below the water
// onset speed or on a dry patch this is a no-op (returns 1).
f32 AquaplaneGrip(f32 wading_depth, f32 wheel_radius, f32 speed) {
  if (wading_depth <= 0.0f || wheel_radius <= 0.0f) return 1.0f;
  const f32 depth_frac = std::min(wading_depth / (0.5f * wheel_radius), 1.0f);
  constexpr f32 kOnset = 8.0f;
  constexpr f32 kFull = 25.0f;
  const f32 speed_frac = std::clamp((speed - kOnset) / (kFull - kOnset), 0.0f, 1.0f);
  return 1.0f - 0.9f * depth_frac * speed_frac;
}

// Maps normalized (rpm-fraction, torque-fraction) points onto Jolt's engine
// torque curve. `count` 0 leaves Jolt's stock curve untouched.
template <class Point>
void ApplyTorqueCurve(JPH::VehicleEngineSettings& engine, const Point* curve, u32 count) {
  if (count == 0) return;
  engine.mNormalizedTorque.Clear();
  engine.mNormalizedTorque.Reserve(count);
  for (u32 i = 0; i < count; ++i) {
    engine.mNormalizedTorque.AddPoint(std::clamp(curve[i].rpm_fraction, 0.0f, 1.0f),
                                      std::max(curve[i].torque_fraction, 0.0f));
  }
  engine.mNormalizedTorque.Sort();
}

// High-speed steering fade: 1 at rest, easing to `fraction` as |speed| reaches
// `fade_speed` (m/s), then held. fraction >= 1 or fade_speed <= 0 = no fade.
f32 SteerFadeScale(f32 fraction, f32 fade_speed, f32 speed) {
  if (fade_speed <= 0.0f || fraction >= 1.0f) return 1.0f;
  const f32 t = std::clamp(std::fabs(speed) / fade_speed, 0.0f, 1.0f);
  return 1.0f - (1.0f - fraction) * t;
}

}  // namespace

struct PhysicsWorld::Impl {
  BroadPhaseLayers broad_phase_layers;
  ObjectVsBroadPhase object_vs_broad_phase;
  ObjectLayerPair object_layer_pair;
  std::unique_ptr<JPH::TempAllocator> temp_allocator;
  JPH::TempAllocatorImplWithMallocFallback skin_allocator{1024 * 1024};
  std::unique_ptr<JPH::JobSystemThreadPool> job_system;
  std::unique_ptr<JPH::PhysicsSystem> system;
  base::Vector<JPH::BodyID> dynamic_bodies;
  base::UnorderedMap<u64, JPH::RefConst<JPH::Shape>> mesh_shapes;
  // One PhysicsMaterial per SurfaceType, created lazily. Installed on tagged
  // static colliders so the tire-friction callback can resolve a surface.
  JPH::RefConst<JPH::PhysicsMaterial> surface_materials[kSurfaceCount];
  // Per-body surface for shared-shape instances (AddStaticMeshInstance), whose
  // shape can't carry a per-instance material; keyed by BodyID index+sequence.
  base::UnorderedMap<u32, SurfaceType> body_surface;
  // Bodies opted out of the generic Update buoyancy (force-based hulls that run
  // their own multi-point model); keyed by BodyID index+sequence.
  base::UnorderedMap<u32, bool> buoyancy_exempt;

  // Lazily builds and returns the material for `surface`; null for asphalt so
  // untagged/asphalt colliders keep the stock (materialless) shape.
  const JPH::PhysicsMaterial* material_for(SurfaceType surface) {
    if (surface == SurfaceType::kAsphalt) return nullptr;
    const u32 i = static_cast<u32>(surface);
    if (i >= kSurfaceCount) return nullptr;
    if (!surface_materials[i]) {
      surface_materials[i] = new JPH::PhysicsMaterialSimple(SurfaceName(surface), JPH::Color::sGrey);
    }
    return surface_materials[i].GetPtr();
  }
  // Resolves a shape's material back to a SurfaceType by pointer identity
  // against the palette (no RTTI); asphalt when untagged or unknown.
  SurfaceType surface_of(const JPH::PhysicsMaterial* m) const {
    if (m) {
      for (u32 i = 0; i < kSurfaceCount; ++i) {
        if (surface_materials[i].GetPtr() == m) return static_cast<SurfaceType>(i);
      }
    }
    return SurfaceType::kAsphalt;
  }
  base::Vector<JPH::Ref<JPH::GroupFilterTable>> filter_groups;
  struct CharacterEntry {
    JPH::Ref<JPH::CharacterVirtual> character;
    f32 vy = 0;           // tracked vertical velocity (gravity + jump)
    f32 step_height = 0.4f;  // stair-walk lift honoured by the Move* calls
  };
  base::Vector<CharacterEntry> characters;
  // Typed handles to created joints so the motor/pose API can address them;
  // JointId is index + 1. `hinge` selects the downcast (HingeConstraint vs
  // SwingTwistConstraint) at use.
  struct JointEntry {
    JPH::Ref<JPH::Constraint> constraint;
    bool hinge = false;
  };
  base::Vector<JointEntry> joints;
  // Wheeled vehicles; VehicleId is index + 1, dead entries keep their slot so
  // handles stay stable.
  struct VehicleEntry {
    JPH::BodyID body;
    JPH::Ref<JPH::VehicleConstraint> constraint;
    bool alive = false;
    u32 wheel_count = 4;
    f32 downforce = 0;  // N per (m/s)^2, applied along body -Y
    // Downforce balance + axle offsets so the aero load can be split front/rear
    // at the axles rather than all at the CoM (0.5 = the legacy single force).
    f32 downforce_balance = 0.5f;
    f32 front_z = 0;
    f32 rear_z = 0;
    // High-speed steering fade applied to the steer command in DriveVehicle.
    f32 steer_high_speed_fraction = 1.0f;
    f32 steer_fade_speed = 0;
    bool traction_control = false;
    f32 tc_scale = 1;  // smoothed traction-control throttle authority
    // Manual transmission state: gears change only on the shift edges of the
    // VehicleInput overload; prev_* debounce those edges.
    bool manual = false;
    bool prev_shift_up = false;
    bool prev_shift_down = false;
    // Chassis speed cached at the top of each Update, read by the tire-friction
    // combine callback (which runs inside the step and can't safely re-lock the
    // body). One step of lag is irrelevant to the aquaplaning ramp.
    f32 cached_speed = 0;
  };
  base::Vector<VehicleEntry> vehicles;
  // Strand grooms (soft-body Cosserat rods); StrandGroomId is index + 1, dead
  // entries keep their slot so handles stay stable.
  struct StrandGroomEntry {
    JPH::BodyID body;
    JPH::BodyID proxy;  // kinematic character collision proxy; may be invalid
    bool alive = false;
    u32 node_count = 0;
    f32 total_mass = 0;
    JPH::Vec3 wind = JPH::Vec3::sZero();
    // Pinned nodes (roots + style pins) and their current body-local targets;
    // Update() pulls each one to its target over the step.
    base::Vector<u32> pinned;
    base::Vector<Vec3> pinned_rest;  // groom-local rest, retargeted by transform
    base::Vector<JPH::Vec3> targets;
  };
  base::Vector<StrandGroomEntry> strand_grooms;
  // Triangle cloth; ClothId is index + 1 and dead slots are retained so
  // handles never alias a later instance. TODO: pack generation + index into
  // ClothId so dead slots can be reused without accepting stale handles.
  struct ClothEntry {
    JPH::BodyID body;
    bool alive = false;
    bool pressure_capable = false;
    u32 vertex_count = 0;
    u32 joint_count = 0;
    u32 skin_constraint_count = 0;
    f32 aerodynamic_drag = 0;
    f32 gravity_factor = 1;
    f32 max_linear_velocity = 100;
    f32 pin_time_remaining = 0;
    JPH::Vec3 wind = JPH::Vec3::sZero();
    base::Vector<u32> pinned;
    base::Vector<Vec3> pinned_rest;
    base::Vector<Vec3> targets;  // persistent world-space targets
    base::Vector<Vec3> target_scratch;
    base::Vector<Mat4> last_joint_transforms;
    base::Vector<JPH::Mat44> skin_pose;
    detail::ClothTopology topology;
    detail::ClothSelfCollisionConfig self_collision;
    detail::ClothSelfCollisionScratch self_collision_scratch;
    base::Vector<Vec3> collision_positions;
    base::Vector<Vec3> collision_velocities;
    base::Vector<f32> collision_inverse_masses;
  };
  base::Vector<ClothEntry> cloth;
};

PhysicsWorld::PhysicsWorld() = default;

PhysicsWorld::~PhysicsWorld() {
  if (impl_) {
    impl_.reset();
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
  }
}

bool PhysicsWorld::Initialize() {
  JPH::RegisterDefaultAllocator();
  JPH::Trace = TraceCallback;
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();

  impl_ = std::make_unique<Impl>();
  // Large cloth batches can exceed the fixed arena. The normal path remains a
  // lock-free stack allocation; exceptional peaks fall back instead of aborting.
  impl_->temp_allocator =
      std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(32 * 1024 * 1024);
  impl_->job_system = std::make_unique<JPH::JobSystemThreadPool>(
      JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
      static_cast<int>(std::thread::hardware_concurrency()) - 1);

  impl_->system = std::make_unique<JPH::PhysicsSystem>();
  impl_->system->Init(65536, 0, 65536, 10240, impl_->broad_phase_layers,
                      impl_->object_vs_broad_phase, impl_->object_layer_pair);
  impl_->system->SetGravity({0, -9.81f, 0});
  RX_INFO("jolt physics initialized");
  return true;
}

void PhysicsWorld::Update(f32 dt) {
  if (!impl_) return;

  // Buoyancy before the step, the Jolt water sample scheme: any awake
  // dynamic body below its local water surface gets buoyancy + drag.
  if (water_height_) {
    JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
    for (JPH::BodyID id : impl_->dynamic_bodies) {
      // Force-based hulls (boats) run their own multi-point buoyancy; skip the
      // generic scheme for them so the two don't stack.
      if (impl_->buoyancy_exempt.find(id.GetIndexAndSequenceNumber())) continue;
      JPH::RVec3 position = bodies.GetCenterOfMassPosition(id);
      f32 surface = 0;
      Vec3 flow{};
      if (!water_height_({static_cast<f32>(position.GetX()), static_cast<f32>(position.GetY()),
                          static_cast<f32>(position.GetZ())},
                         &surface, &flow)) {
        continue;
      }
      // Moving water never lets floaters sleep; still water does.
      bool in_flow = (flow.x != 0 || flow.y != 0 || flow.z != 0) && position.GetY() < surface;
      if (!bodies.IsActive(id)) {
        if (!in_flow) continue;
        bodies.ActivateBody(id);
      }
      JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), id);
      if (!lock.Succeeded()) continue;
      JPH::Body& body = lock.GetBody();
      body.ApplyBuoyancyImpulse(JPH::RVec3(position.GetX(), surface, position.GetZ()),
                                JPH::Vec3::sAxisY(), 1.2f, 0.5f, 0.05f, ToJolt(flow),
                                impl_->system->GetGravity(), dt);
    }
  }

  // Cache each vehicle's chassis speed for the in-step tire-friction callback.
  {
    JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
    for (Impl::VehicleEntry& entry : impl_->vehicles) {
      if (!entry.alive) continue;
      entry.cached_speed = bodies.GetLinearVelocity(entry.body).Length();
    }
  }

  // Aero downforce: presses racing vehicles into the road along their body
  // -Y (so banked corners keep the full benefit), quadratic in forward speed.
  {
    JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
    for (const Impl::VehicleEntry& entry : impl_->vehicles) {
      if (!entry.alive || entry.downforce <= 0 || !bodies.IsActive(entry.body)) continue;
      const JPH::Quat rotation = bodies.GetRotation(entry.body);
      const JPH::Vec3 forward = rotation * JPH::Vec3::sAxisZ();
      const JPH::Vec3 up = rotation * JPH::Vec3::sAxisY();
      const f32 v = bodies.GetLinearVelocity(entry.body).Dot(forward);
      const JPH::Vec3 press = up * (-entry.downforce * v * v);
      // An even balance keeps the legacy single force at the CoM; a front/rear
      // split presses each axle so front-biased downforce grows front grip and
      // the asymmetry adds a small aero pitch. The force is vertical, so only
      // the fore/aft (Z) arm produces a couple - the axle height is irrelevant.
      if (entry.downforce_balance == 0.5f || (entry.front_z == 0 && entry.rear_z == 0)) {
        bodies.AddForce(entry.body, press);
      } else {
        const JPH::RVec3 com = bodies.GetCenterOfMassPosition(entry.body);
        const f32 fb = std::clamp(entry.downforce_balance, 0.0f, 1.0f);
        bodies.AddForce(entry.body, press * fb, com + rotation * JPH::Vec3(0, 0, entry.front_z));
        bodies.AddForce(entry.body, press * (1.0f - fb),
                        com + rotation * JPH::Vec3(0, 0, entry.rear_z));
      }
    }
  }

  // Wheel water drag: a wheel whose contact patch is under the water surface
  // drags against the water it has to plough through. Force ~ 0.5*rho*Cd*A*v^2
  // opposing the contact point's motion relative to the flow, with A the
  // submerged frontal area (width * wading depth, capped at the wheel), applied
  // at the contact point. Horizontal only and magnitude-capped so hitting a
  // ford decelerates the car without exploding the solver. Grip loss itself is
  // handled in the tire-friction combine (aquaplaning); this is the bulk drag.
  if (water_height_) {
    JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
    constexpr f32 kWaterDensity = 1000.0f;
    constexpr f32 kWheelDragCd = 1.0f;
    for (const Impl::VehicleEntry& entry : impl_->vehicles) {
      if (!entry.alive || !bodies.IsActive(entry.body)) continue;
      for (u32 i = 0; i < entry.wheel_count; ++i) {
        const auto* wheel = static_cast<const JPH::WheelWV*>(entry.constraint->GetWheel(i));
        if (!wheel->HasContact()) continue;
        const JPH::RVec3 cp = wheel->GetContactPosition();
        const Vec3 p{static_cast<f32>(cp.GetX()), static_cast<f32>(cp.GetY()),
                     static_cast<f32>(cp.GetZ())};
        f32 surface_h = 0;
        Vec3 flow{};
        if (!SampleWater(p, &surface_h, &flow) || p.y >= surface_h) continue;
        const f32 depth = std::min(surface_h - p.y, 2.0f * wheel->GetSettings()->mRadius);
        const JPH::Vec3 point_vel = wheel->GetContactPointVelocity();
        JPH::Vec3 rel(point_vel.GetX() - flow.x, 0.0f, point_vel.GetZ() - flow.z);
        const f32 speed = rel.Length();
        if (speed < 0.1f) continue;
        const f32 area = wheel->GetSettings()->mWidth * depth;
        f32 mag = 0.5f * kWaterDensity * kWheelDragCd * area * speed * speed;
        mag = std::min(mag, 8000.0f);  // per-wheel cap keeps a deep, fast entry stable
        const JPH::Vec3 force = rel * (-mag / speed);
        bodies.AddForce(entry.body, force, cp);
      }
    }
  }

  // Strand grooms: pull every pinned node to its retargeted position over
  // this step (kinematic soft-body vertices integrate their velocity) and
  // apply wind as a whole-body force. Sleeping grooms are settled and skip.
  if (dt > 0) {
    JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
    const f32 inv_dt = 1.0f / dt;
    for (Impl::StrandGroomEntry& entry : impl_->strand_grooms) {
      if (!entry.alive || !bodies.IsActive(entry.body)) continue;
      if (!entry.wind.IsNearZero()) {
        bodies.AddForce(entry.body, entry.wind * entry.total_mass);
      }
      JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), entry.body);
      if (!lock.Succeeded()) continue;
      auto* soft =
          static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
      for (size_t i = 0; i < entry.pinned.size(); ++i) {
        JPH::SoftBodyVertex& v = soft->GetVertex(entry.pinned[i]);
        v.mVelocity = (entry.targets[i] - v.mPosition) * inv_dt;
      }
    }
  }

  // Cloth attachments, aerodynamic drag and rx's self-collision extension are
  // velocity-only prepasses. Jolt then integrates them together with its native
  // XPBD constraints and rigid contacts in the parallel soft-body jobs.
  if (dt > 0) {
    constexpr f32 kAirDensity = 1.225f;
    JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
    for (Impl::ClothEntry& entry : impl_->cloth) {
      if (!entry.alive || !bodies.IsActive(entry.body)) continue;
      JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), entry.body);
      if (!lock.Succeeded()) continue;
      JPH::Body& body = lock.GetBody();
      auto* soft = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
      JPH::Array<JPH::SoftBodyVertex>& vertices = soft->GetVertices();

      const JPH::Mat44 to_local =
          body.GetCenterOfMassTransform().InversedRotationTranslation().ToMat44();
      const f32 pin_duration = std::max(entry.pin_time_remaining, dt);
      for (size_t i = 0; i < entry.pinned.size(); ++i) {
        JPH::SoftBodyVertex& vertex = vertices[entry.pinned[i]];
        const JPH::Vec3 target = to_local * ToJolt(entry.targets[i]);
        vertex.mVelocity = (target - vertex.mPosition) / pin_duration;
      }
      entry.pin_time_remaining = std::max(entry.pin_time_remaining - dt, 0.0f);

      if (entry.aerodynamic_drag > 0) {
        const JPH::Quat inverse_rotation = body.GetRotation().Conjugated();
        const JPH::Vec3 local_wind = inverse_rotation * entry.wind;
        for (size_t i = 0; i < entry.topology.indices.size(); i += 3) {
          const u32 ia = entry.topology.indices[i + 0];
          const u32 ib = entry.topology.indices[i + 1];
          const u32 ic = entry.topology.indices[i + 2];
          JPH::SoftBodyVertex& a = vertices[ia];
          JPH::SoftBodyVertex& b = vertices[ib];
          JPH::SoftBodyVertex& c = vertices[ic];
          const JPH::Vec3 area_normal =
              (b.mPosition - a.mPosition).Cross(c.mPosition - a.mPosition);
          const f32 area_twice = area_normal.Length();
          if (area_twice < 1.0e-8f) continue;
          const JPH::Vec3 normal = area_normal / area_twice;
          const JPH::Vec3 cloth_velocity = (a.mVelocity + b.mVelocity + c.mVelocity) / 3.0f;
          const f32 normal_speed = (local_wind - cloth_velocity).Dot(normal);
          const JPH::Vec3 force = normal * (0.25f * kAirDensity * entry.aerodynamic_drag *
                                             area_twice * normal_speed * std::abs(normal_speed));
          if (!std::isfinite(force.GetX()) || !std::isfinite(force.GetY()) ||
              !std::isfinite(force.GetZ())) {
            continue;
          }
          for (JPH::SoftBodyVertex* vertex : {&a, &b, &c}) {
            JPH::Vec3 delta = force * (vertex->mInvMass * dt / 3.0f);
            const f32 delta_sq = delta.LengthSq();
            // Explicit quadratic drag must not overshoot the air velocity in
            // one face contribution or it can oscillate and hit the speed cap.
            const f32 max_delta =
                std::min(entry.max_linear_velocity * 0.25f, std::abs(normal_speed));
            if (delta_sq > max_delta * max_delta) delta *= max_delta / std::sqrt(delta_sq);
            vertex->mVelocity += delta;
            const f32 speed_sq = vertex->mVelocity.LengthSq();
            if (speed_sq > entry.max_linear_velocity * entry.max_linear_velocity) {
              vertex->mVelocity *= entry.max_linear_velocity / std::sqrt(speed_sq);
            }
          }
        }
      }

      if (entry.self_collision.distance > 0) {
        const JPH::Vec3 gravity_delta = body.GetRotation().Conjugated() *
                                        impl_->system->GetGravity() * (entry.gravity_factor * dt);
        entry.collision_positions.resize(vertices.size());
        entry.collision_velocities.resize(vertices.size());
        for (size_t i = 0; i < vertices.size(); ++i) {
          entry.collision_positions[i] = {vertices[i].mPosition.GetX(),
                                          vertices[i].mPosition.GetY(),
                                          vertices[i].mPosition.GetZ()};
          entry.collision_velocities[i] = {vertices[i].mVelocity.GetX(),
                                           vertices[i].mVelocity.GetY(),
                                           vertices[i].mVelocity.GetZ()};
          if (vertices[i].mInvMass > 0) {
            entry.collision_velocities[i] +=
                Vec3{gravity_delta.GetX(), gravity_delta.GetY(), gravity_delta.GetZ()};
          }
        }
        detail::SolveClothSelfCollision(
            entry.topology, entry.self_collision, entry.collision_positions,
            &entry.collision_velocities, entry.collision_inverse_masses, dt,
            &entry.self_collision_scratch);
        for (size_t i = 0; i < vertices.size(); ++i) {
          vertices[i].mVelocity = ToJolt(entry.collision_velocities[i]);
          if (vertices[i].mInvMass > 0) vertices[i].mVelocity -= gravity_delta;
        }
      }
    }
  }

  const JPH::EPhysicsUpdateError error =
      impl_->system->Update(dt, 1, impl_->temp_allocator.get(), impl_->job_system.get());
  if (error != JPH::EPhysicsUpdateError::None) {
    RX_WARN("jolt update capacity error mask: {}", static_cast<u32>(error));
  }
}

BodyId PhysicsWorld::AddStaticBox(const Vec3& position, const Vec3& half_extent,
                                  SurfaceType surface) {
  if (!impl_) return 0;
  JPH::Ref<JPH::BoxShape> box = new JPH::BoxShape(ToJolt(half_extent));
  box->SetMaterial(impl_->material_for(surface));
  JPH::BodyCreationSettings settings(box, ToJolt(position), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Static, layers::kStatic);
  settings.mFriction = kStaticFriction;
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddStaticMesh(const asset::Mesh& mesh, const Vec3& position,
                                   const f32 rotation[4], f32 scale, SurfaceType surface) {
  if (!impl_ || mesh.lods.empty()) return 0;
  const asset::MeshLod& lod = mesh.lods[0];
  if (lod.indices.size() < 3) return 0;

  JPH::VertexList vertices;
  vertices.reserve(lod.vertices.size());
  for (const asset::Vertex& v : lod.vertices) {
    vertices.push_back({v.position[0], v.position[1], v.position[2]});
  }
  JPH::IndexedTriangleList triangles;
  triangles.reserve(lod.indices.size() / 3);
  for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) {
    triangles.push_back({lod.indices[i], lod.indices[i + 1], lod.indices[i + 2], 0});
  }
  JPH::PhysicsMaterialList materials;
  if (const JPH::PhysicsMaterial* m = impl_->material_for(surface)) materials.push_back(m);
  JPH::Ref<JPH::ShapeSettings> shape_settings =
      new JPH::MeshShapeSettings(std::move(vertices), std::move(triangles), std::move(materials));
  if (scale != 1.0f) {
    shape_settings = new JPH::ScaledShapeSettings(shape_settings, JPH::Vec3::sReplicate(scale));
  }
  JPH::ShapeSettings::ShapeResult result = shape_settings->Create();
  if (result.HasError()) {
    RX_WARN("mesh collider failed: {}", result.GetError().c_str());
    return 0;
  }
  JPH::BodyCreationSettings settings(result.Get(), ToJolt(position),
                                     JPH::Quat(rotation[0], rotation[1], rotation[2], rotation[3]),
                                     JPH::EMotionType::Static, layers::kStatic);
  settings.mFriction = kStaticFriction;
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

bool PhysicsWorld::RegisterMeshShape(u64 key, const asset::Mesh& mesh) {
  if (!impl_ || mesh.lods.empty()) return false;
  if (impl_->mesh_shapes.find(key)) return true;
  const asset::MeshLod& lod = mesh.lods[0];
  if (lod.indices.size() < 3) return false;

  JPH::VertexList vertices;
  vertices.reserve(lod.vertices.size());
  for (const asset::Vertex& v : lod.vertices) {
    vertices.push_back({v.position[0], v.position[1], v.position[2]});
  }
  JPH::IndexedTriangleList triangles;
  triangles.reserve(lod.indices.size() / 3);
  for (size_t i = 0; i + 2 < lod.indices.size(); i += 3) {
    triangles.push_back({lod.indices[i], lod.indices[i + 1], lod.indices[i + 2], 0});
  }
  JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
  JPH::ShapeSettings::ShapeResult result = settings.Create();
  if (result.HasError()) return false;
  impl_->mesh_shapes.insert(key, result.Get());
  return true;
}

bool PhysicsWorld::has_mesh_shape(u64 key) const {
  return impl_ && impl_->mesh_shapes.find(key) != nullptr;
}

BodyId PhysicsWorld::AddStaticMeshInstance(u64 key, const Vec3& position, const f32 rotation[4],
                                           f32 scale, SurfaceType surface) {
  if (!impl_) return 0;
  const JPH::RefConst<JPH::Shape>* shape = impl_->mesh_shapes.find(key);
  if (!shape) return 0;
  JPH::RefConst<JPH::Shape> instance = *shape;
  if (scale != 1.0f) {
    instance = new JPH::ScaledShape(instance, JPH::Vec3::sReplicate(scale));
  }
  JPH::BodyCreationSettings settings(instance, ToJolt(position),
                                     JPH::Quat(rotation[0], rotation[1], rotation[2], rotation[3]),
                                     JPH::EMotionType::Static, layers::kStatic);
  settings.mFriction = kStaticFriction;
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  // The mesh shape is shared across instances, so a per-instance material can't
  // ride on it; record the surface per body for the tire callback instead.
  if (surface != SurfaceType::kAsphalt) {
    impl_->body_surface.insert(id.GetIndexAndSequenceNumber(), surface);
  }
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddHeightField(const Vec3& origin, const f32* heights, u32 samples, f32 size,
                                    SurfaceType surface) {
  return AddHeightField(origin, heights, samples, size, nullptr,
                        surface == SurfaceType::kAsphalt ? nullptr : &surface,
                        surface == SurfaceType::kAsphalt ? 0 : 1);
}

BodyId PhysicsWorld::AddHeightField(const Vec3& origin, const f32* heights, u32 samples, f32 size,
                                    const u8* material_indices, const SurfaceType* palette,
                                    u32 palette_count) {
  if (!impl_ || samples < 2) return 0;
  JPH::HeightFieldShapeSettings shape(heights, ToJolt(origin),
                                      {size / static_cast<f32>(samples - 1), 1.0f,
                                       size / static_cast<f32>(samples - 1)},
                                      samples);
  // Build the material palette + per-quad indices so a cell can mix surfaces
  // (asphalt road over grass). A single non-asphalt surface with no explicit
  // index map paints the whole cell. Asphalt-only stays materialless (stock).
  if (palette && palette_count > 0) {
    for (u32 i = 0; i < palette_count; ++i) {
      const JPH::PhysicsMaterial* m = impl_->material_for(palette[i]);
      // The palette must stay index-aligned; asphalt yields no material, so
      // give it a real (grey) material entry to hold its slot.
      shape.mMaterials.push_back(
          m ? JPH::RefConst<JPH::PhysicsMaterial>(m)
            : JPH::RefConst<JPH::PhysicsMaterial>(
                  new JPH::PhysicsMaterialSimple(SurfaceName(SurfaceType::kAsphalt), JPH::Color::sGrey)));
    }
    const u32 quads = (samples - 1) * (samples - 1);
    if (material_indices) {
      shape.mMaterialIndices.assign(material_indices, material_indices + quads);
    } else {
      shape.mMaterialIndices.resize(quads, 0);  // single-surface fill
    }
  }
  JPH::ShapeSettings::ShapeResult result = shape.Create();
  if (result.HasError()) {
    RX_WARN("heightfield collider failed: {}", result.GetError().c_str());
    return 0;
  }
  JPH::BodyCreationSettings settings(result.Get(), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Static, layers::kStatic);
  settings.mFriction = kStaticFriction;
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

namespace {

// Quaternion from three orthonormal basis COLUMNS (c0, c1, c2), the layout
// ShapeDesc::transform carries.
JPH::Quat QuatFromColumns(const f32* t) {
  JPH::Mat44 m = JPH::Mat44::sIdentity();
  m.SetColumn3(0, {t[0], t[1], t[2]});
  m.SetColumn3(1, {t[4], t[5], t[6]});
  m.SetColumn3(2, {t[8], t[9], t[10]});
  return m.GetQuaternion().Normalized();
}

// Lowers a ShapeDesc tree into a Jolt shape, scaling all metrics into
// meters. Returns null on unconvertible input (empty hulls, degenerate
// primitives), which callers surface as a failed body.
JPH::Ref<JPH::Shape> BuildShape(const rx::physics::ShapeDesc& desc, f32 scale,
                                const JPH::PhysicsMaterial* material = nullptr) {
  using Kind = rx::physics::ShapeDesc::Kind;
  switch (desc.kind) {
    case Kind::kSphere: {
      if (desc.radius * scale < 1e-4f) return nullptr;
      return new JPH::SphereShape(desc.radius * scale, material);
    }
    case Kind::kCapsule: {
      // Havok capsules are arbitrary segments; Jolt's are Y-centered, so
      // wrap in a rotated-translated placement.
      JPH::Vec3 a(desc.a.x, desc.a.y, desc.a.z);
      JPH::Vec3 b(desc.b.x, desc.b.y, desc.b.z);
      a *= scale;
      b *= scale;
      f32 radius = desc.radius * scale;
      f32 half_height = (b - a).Length() * 0.5f;
      if (radius < 1e-4f) return nullptr;
      if (half_height < 1e-4f) return new JPH::SphereShape(radius, material);
      JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(half_height, radius, material);
      JPH::Vec3 axis = (b - a).Normalized();
      JPH::Quat align = JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), axis);
      JPH::RotatedTranslatedShapeSettings placed((a + b) * 0.5f, align, capsule);
      auto result = placed.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kBox: {
      JPH::Vec3 half(desc.half_extents.x, desc.half_extents.y, desc.half_extents.z);
      half *= scale;
      f32 min_extent = half.ReduceMin();
      if (min_extent < 1e-4f) return nullptr;
      return new JPH::BoxShape(half, JPH::min(0.05f, min_extent * 0.5f), material);
    }
    case Kind::kConvexHull: {
      if (desc.vertices.size() < 4) return nullptr;
      JPH::Array<JPH::Vec3> points;
      points.reserve(desc.vertices.size());
      for (const auto& v : desc.vertices) points.emplace_back(v.x * scale, v.y * scale,
                                                              v.z * scale);
      JPH::ConvexHullShapeSettings hull(points, JPH::cDefaultConvexRadius, material);
      auto result = hull.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kCompound: {
      JPH::StaticCompoundShapeSettings compound;
      u32 added = 0;
      for (const auto& child : desc.children) {
        if (child.kind == Kind::kPlaced && !child.children.empty()) {
          JPH::Ref<JPH::Shape> inner = BuildShape(child.children[0], scale, material);
          if (!inner) continue;
          JPH::Vec3 origin(child.transform[12], child.transform[13], child.transform[14]);
          compound.AddShape(origin * scale, QuatFromColumns(child.transform), inner);
          ++added;
        } else if (JPH::Ref<JPH::Shape> inner = BuildShape(child, scale, material)) {
          compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), inner);
          ++added;
        }
      }
      if (added == 0) return nullptr;
      if (added == 1 && desc.children.size() == 1 &&
          desc.children[0].kind != Kind::kPlaced) {
        return BuildShape(desc.children[0], scale, material);  // trivial list
      }
      auto result = compound.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kPlaced: {
      if (desc.children.empty()) return nullptr;
      JPH::Ref<JPH::Shape> inner = BuildShape(desc.children[0], scale, material);
      if (!inner) return nullptr;
      JPH::Vec3 origin(desc.transform[12], desc.transform[13], desc.transform[14]);
      JPH::RotatedTranslatedShapeSettings placed(origin * scale, QuatFromColumns(desc.transform),
                                                 inner);
      auto result = placed.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kInvalid:
      return nullptr;
  }
  return nullptr;
}

}  // namespace

BodyId PhysicsWorld::AddStaticShape(const ShapeDesc& desc, const Vec3& position,
                                    const f32 rotation[4], f32 scale, SurfaceType surface) {
  if (!impl_) return 0;
  JPH::Ref<JPH::Shape> shape = BuildShape(desc, scale, impl_->material_for(surface));
  if (!shape) return 0;
  JPH::Quat rot(rotation[0], rotation[1], rotation[2], rotation[3]);
  if (rot.LengthSq() < 1e-6f) rot = JPH::Quat::sIdentity();
  JPH::BodyCreationSettings settings(shape, ToJolt(position), rot.Normalized(),
                                     JPH::EMotionType::Static, layers::kStatic);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

i32 PhysicsWorld::CreateBodyFilterGroup(u32 subgroup_count) {
  if (!impl_) return -1;
  impl_->filter_groups.push_back(new JPH::GroupFilterTable(subgroup_count));
  return static_cast<i32>(impl_->filter_groups.size()) - 1;
}

void PhysicsWorld::DisableFilterPair(i32 group, u32 sub_a, u32 sub_b) {
  if (!impl_ || group < 0 || group >= static_cast<i32>(impl_->filter_groups.size())) return;
  impl_->filter_groups[group]->DisableCollision(sub_a, sub_b);
}

BodyId PhysicsWorld::AddDynamicShape(const ShapeDesc& desc, const Vec3& position,
                                     const f32 rotation[4], f32 scale, f32 mass, f32 friction,
                                     f32 restitution, i32 filter_group, u32 subgroup) {
  if (!impl_) return 0;
  JPH::Ref<JPH::Shape> shape = BuildShape(desc, scale);
  if (!shape) return 0;
  JPH::Quat rot(rotation[0], rotation[1], rotation[2], rotation[3]);
  if (rot.LengthSq() < 1e-6f) rot = JPH::Quat::sIdentity();
  JPH::BodyCreationSettings settings(shape, ToJolt(position), rot.Normalized(),
                                     JPH::EMotionType::Dynamic, layers::kDynamic);
  settings.mFriction = friction;
  settings.mRestitution = restitution;
  if (filter_group >= 0 && filter_group < static_cast<i32>(impl_->filter_groups.size())) {
    settings.mCollisionGroup = JPH::CollisionGroup(
        impl_->filter_groups[filter_group], static_cast<JPH::CollisionGroup::GroupID>(filter_group),
        static_cast<JPH::CollisionGroup::SubGroupID>(subgroup));
  }
  if (mass > 0.0f) {
    settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = mass;
  }
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  impl_->dynamic_bodies.push_back(id);
  ++dynamic_count_;
  return id.GetIndexAndSequenceNumber() + 1;
}

namespace {

struct JointFrame {
  JPH::RVec3 position;   // world-space pivot
  JPH::Vec3 twist_axis;  // world-space column-0 axis
  JPH::Vec3 plane_axis;  // world-space column-1 axis
};

// Converts a 3x4 row-major body-local frame (basis rows, origin in column 3)
// into world space through the body's current transform.
JointFrame FrameToWorld(JPH::BodyInterface& bodies, JPH::BodyID body, const f32 frame[12],
                        f32 scale) {
  JPH::RVec3 body_pos;
  JPH::Quat body_rot;
  bodies.GetPositionAndRotation(body, body_pos, body_rot);
  JPH::Vec3 local_origin(frame[3], frame[7], frame[11]);
  JPH::Vec3 twist(frame[0], frame[4], frame[8]);
  JPH::Vec3 plane(frame[1], frame[5], frame[9]);
  JointFrame out;
  out.position = body_pos + body_rot * (local_origin * scale);
  out.twist_axis = (body_rot * twist).NormalizedOr(JPH::Vec3::sAxisX());
  out.plane_axis = (body_rot * plane).NormalizedOr(JPH::Vec3::sAxisY());
  return out;
}

JPH::Body* LockBody(JPH::PhysicsSystem& system, JPH::BodyID id) {
  return system.GetBodyLockInterfaceNoLock().TryGetBody(id);
}

}  // namespace

JointId PhysicsWorld::AddSwingTwistJoint(BodyId a, BodyId b, const f32 frame_a[12],
                                         const f32 frame_b[12], f32 scale, f32 twist_min,
                                         f32 twist_max, f32 cone_max, f32 plane_min,
                                         f32 plane_max) {
  if (!impl_ || a == 0 || b == 0) return 0;
  JPH::BodyID body_a(static_cast<JPH::uint32>(a - 1));
  JPH::BodyID body_b(static_cast<JPH::uint32>(b - 1));
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();

  // Both frames describe the same joint; body A's is the authoritative
  // world anchor at setup time (bodies are spawned in their bind pose).
  JointFrame world = FrameToWorld(bodies, body_a, frame_a, scale);
  (void)frame_b;

  JPH::SwingTwistConstraintSettings settings;
  settings.mSpace = JPH::EConstraintSpace::WorldSpace;
  settings.mPosition1 = settings.mPosition2 = world.position;
  settings.mTwistAxis1 = settings.mTwistAxis2 = world.twist_axis;
  settings.mPlaneAxis1 = settings.mPlaneAxis2 = world.plane_axis;
  settings.mTwistMinAngle = twist_min;
  settings.mTwistMaxAngle = twist_max;
  settings.mNormalHalfConeAngle = JPH::max(cone_max, 0.01f);
  settings.mPlaneHalfConeAngle =
      JPH::max(JPH::max(JPH::abs(plane_min), JPH::abs(plane_max)), 0.01f);

  JPH::Body* pa = LockBody(*impl_->system, body_a);
  JPH::Body* pb = LockBody(*impl_->system, body_b);
  if (!pa || !pb) return 0;
  JPH::TwoBodyConstraint* constraint = settings.Create(*pa, *pb);
  impl_->system->AddConstraint(constraint);
  Impl::JointEntry entry;
  entry.constraint = constraint;
  entry.hinge = false;
  impl_->joints.push_back(std::move(entry));
  return static_cast<JointId>(impl_->joints.size());
}

JointId PhysicsWorld::AddHingeJoint(BodyId a, BodyId b, const f32 frame_a[12],
                                    const f32 frame_b[12], f32 scale, f32 angle_min,
                                    f32 angle_max) {
  if (!impl_ || a == 0 || b == 0) return 0;
  JPH::BodyID body_a(static_cast<JPH::uint32>(a - 1));
  JPH::BodyID body_b(static_cast<JPH::uint32>(b - 1));
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  JointFrame world = FrameToWorld(bodies, body_a, frame_a, scale);
  (void)frame_b;

  JPH::HingeConstraintSettings settings;
  settings.mSpace = JPH::EConstraintSpace::WorldSpace;
  settings.mPoint1 = settings.mPoint2 = world.position;
  settings.mHingeAxis1 = settings.mHingeAxis2 = world.twist_axis;
  settings.mNormalAxis1 = settings.mNormalAxis2 = world.plane_axis;
  settings.mLimitsMin = JPH::min(angle_min, angle_max);
  settings.mLimitsMax = JPH::max(angle_min, angle_max);

  JPH::Body* pa = LockBody(*impl_->system, body_a);
  JPH::Body* pb = LockBody(*impl_->system, body_b);
  if (!pa || !pb) return 0;
  JPH::TwoBodyConstraint* constraint = settings.Create(*pa, *pb);
  impl_->system->AddConstraint(constraint);
  Impl::JointEntry entry;
  entry.constraint = constraint;
  entry.hinge = true;
  impl_->joints.push_back(std::move(entry));
  return static_cast<JointId>(impl_->joints.size());
}

void PhysicsWorld::EnableJointMotors(JointId joint, f32 frequency, f32 damping) {
  if (!impl_ || joint == 0 || joint > impl_->joints.size()) return;
  Impl::JointEntry& entry = impl_->joints[joint - 1];
  JPH::SpringSettings spring(JPH::ESpringMode::FrequencyAndDamping, frequency, damping);
  if (entry.hinge) {
    auto* hinge = static_cast<JPH::HingeConstraint*>(entry.constraint.GetPtr());
    hinge->GetMotorSettings().mSpringSettings = spring;
    hinge->SetMotorState(JPH::EMotorState::Position);
  } else {
    auto* st = static_cast<JPH::SwingTwistConstraint*>(entry.constraint.GetPtr());
    st->GetSwingMotorSettings().mSpringSettings = spring;
    st->GetTwistMotorSettings().mSpringSettings = spring;
    st->SetSwingMotorState(JPH::EMotorState::Position);
    st->SetTwistMotorState(JPH::EMotorState::Position);
  }
}

void PhysicsWorld::SetJointMotorTarget(JointId joint, const f32 target_quat[4]) {
  if (!impl_ || joint == 0 || joint > impl_->joints.size()) return;
  Impl::JointEntry& entry = impl_->joints[joint - 1];
  JPH::Quat q(target_quat[0], target_quat[1], target_quat[2], target_quat[3]);
  if (q.LengthSq() < 1e-8f) q = JPH::Quat::sIdentity();
  q = q.Normalized();
  if (entry.hinge) {
    auto* hinge = static_cast<JPH::HingeConstraint*>(entry.constraint.GetPtr());
    // Twist angle about the hinge axis (constraint-space X) of the target.
    f32 angle = 2.0f * std::atan2(q.GetX(), q.GetW());
    hinge->SetTargetAngle(angle);
  } else {
    auto* st = static_cast<JPH::SwingTwistConstraint*>(entry.constraint.GetPtr());
    st->SetTargetOrientationCS(q);
  }
}

bool PhysicsWorld::GetJointOrientation(JointId joint, f32 out_quat[4]) const {
  if (!impl_ || joint == 0 || joint > impl_->joints.size()) return false;
  Impl::JointEntry& entry = impl_->joints[joint - 1];
  JPH::Quat q;
  if (entry.hinge) {
    auto* hinge = static_cast<JPH::HingeConstraint*>(entry.constraint.GetPtr());
    q = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), hinge->GetCurrentAngle());
  } else {
    auto* st = static_cast<JPH::SwingTwistConstraint*>(entry.constraint.GetPtr());
    q = st->GetRotationInConstraintSpace();
  }
  out_quat[0] = q.GetX();
  out_quat[1] = q.GetY();
  out_quat[2] = q.GetZ();
  out_quat[3] = q.GetW();
  return true;
}

void PhysicsWorld::ApplyImpulse(BodyId id, const Vec3& impulse) {
  if (!impl_ || id == 0) return;
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  bodies.ActivateBody(body);
  bodies.AddImpulse(body, ToJolt(impulse));
}

bool PhysicsWorld::SampleWater(const Vec3& position, f32* out_height, Vec3* out_flow) const {
  if (!water_height_) return false;
  f32 height = 0;
  Vec3 flow{};
  if (!water_height_(position, &height, &flow)) return false;
  if (out_height) *out_height = height;
  if (out_flow) *out_flow = flow;
  return true;
}

void PhysicsWorld::set_buoyancy_exempt(BodyId id, bool exempt) {
  if (!impl_ || id == 0) return;
  const u32 key = JPH::BodyID(static_cast<JPH::uint32>(id - 1)).GetIndexAndSequenceNumber();
  if (exempt) {
    impl_->buoyancy_exempt.insert(key, true);
  } else {
    impl_->buoyancy_exempt.erase(key);
  }
}

void PhysicsWorld::AddForce(BodyId id, const Vec3& force) {
  if (!impl_ || id == 0) return;
  impl_->system->GetBodyInterface().AddForce(JPH::BodyID(static_cast<JPH::uint32>(id - 1)),
                                             ToJolt(force));
}

void PhysicsWorld::AddForceAtPoint(BodyId id, const Vec3& force, const Vec3& world_point) {
  if (!impl_ || id == 0) return;
  impl_->system->GetBodyInterface().AddForce(JPH::BodyID(static_cast<JPH::uint32>(id - 1)),
                                             ToJolt(force),
                                             JPH::RVec3(world_point.x, world_point.y, world_point.z));
}

void PhysicsWorld::AddTorque(BodyId id, const Vec3& torque) {
  if (!impl_ || id == 0) return;
  impl_->system->GetBodyInterface().AddTorque(JPH::BodyID(static_cast<JPH::uint32>(id - 1)),
                                              ToJolt(torque));
}

Vec3 PhysicsWorld::GetLinearVelocity(BodyId id) const {
  if (!impl_ || id == 0) return {};
  const JPH::Vec3 v =
      impl_->system->GetBodyInterface().GetLinearVelocity(JPH::BodyID(static_cast<JPH::uint32>(id - 1)));
  return {v.GetX(), v.GetY(), v.GetZ()};
}

Vec3 PhysicsWorld::GetAngularVelocity(BodyId id) const {
  if (!impl_ || id == 0) return {};
  const JPH::Vec3 v = impl_->system->GetBodyInterface().GetAngularVelocity(
      JPH::BodyID(static_cast<JPH::uint32>(id - 1)));
  return {v.GetX(), v.GetY(), v.GetZ()};
}

Vec3 PhysicsWorld::GetPointVelocity(BodyId id, const Vec3& world_point) const {
  if (!impl_ || id == 0) return {};
  const JPH::Vec3 v = impl_->system->GetBodyInterface().GetPointVelocity(
      JPH::BodyID(static_cast<JPH::uint32>(id - 1)),
      JPH::RVec3(world_point.x, world_point.y, world_point.z));
  return {v.GetX(), v.GetY(), v.GetZ()};
}

f32 PhysicsWorld::GetBodyMass(BodyId id) const {
  if (!impl_ || id == 0) return 0;
  JPH::BodyLockRead lock(impl_->system->GetBodyLockInterface(),
                         JPH::BodyID(static_cast<JPH::uint32>(id - 1)));
  if (!lock.Succeeded()) return 0;
  const JPH::Body& body = lock.GetBody();
  if (body.IsStatic() || !body.GetMotionProperties()) return 0;
  const f32 inv_mass = body.GetMotionProperties()->GetInverseMassUnchecked();
  return inv_mass > 0 ? 1.0f / inv_mass : 0;
}

void PhysicsWorld::SetBodyInertia(BodyId id, const Vec3& diagonal_kgm2) {
  if (!impl_ || id == 0) return;
  JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(),
                          JPH::BodyID(static_cast<JPH::uint32>(id - 1)));
  if (!lock.Succeeded()) return;
  JPH::Body& body = lock.GetBody();
  if (body.IsStatic() || !body.GetMotionProperties()) return;
  // Jolt stores the INVERSE inertia diagonal; a non-positive component leaves
  // that axis free (inverse 0). Principal axes are the body axes (identity
  // rotation), so diagonal maps straight onto (body x, y, z).
  auto inv = [](f32 v) { return v > 0.0f ? 1.0f / v : 0.0f; };
  body.GetMotionProperties()->SetInverseInertia(
      JPH::Vec3(inv(diagonal_kgm2.x), inv(diagonal_kgm2.y), inv(diagonal_kgm2.z)),
      JPH::Quat::sIdentity());
}

void PhysicsWorld::SetBodyMass(BodyId id, f32 kg) {
  if (!impl_ || id == 0 || kg <= 0.0f) return;
  JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(),
                          JPH::BodyID(static_cast<JPH::uint32>(id - 1)));
  if (!lock.Succeeded()) return;
  JPH::Body& body = lock.GetBody();
  if (body.IsStatic() || !body.GetMotionProperties()) return;
  JPH::MotionProperties* mp = body.GetMotionProperties();
  const f32 inv_mass = mp->GetInverseMassUnchecked();
  if (inv_mass <= 0.0f) return;  // pinned to infinite mass; leave it be
  // Jolt stores inverse mass and an inverse inertia diagonal. Inertia scales
  // linearly with mass for a fixed distribution, so the inverse inertia scales
  // by old_mass/new_mass; that keeps the body's rotational feel consistent with
  // the new mass instead of leaving a box's old (lighter) inertia behind.
  const f32 old_mass = 1.0f / inv_mass;
  const f32 inertia_scale = old_mass / kg;
  mp->SetInverseMass(1.0f / kg);
  mp->SetInverseInertia(mp->GetInverseInertiaDiagonal() * inertia_scale,
                        mp->GetInertiaRotation());
}

void PhysicsWorld::SetBodyKinematic(BodyId id) {
  if (!impl_ || id == 0) return;
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  impl_->system->GetBodyInterface().SetMotionType(body, JPH::EMotionType::Kinematic,
                                                  JPH::EActivation::Activate);
}

BodyId PhysicsWorld::AddDynamicBox(const Vec3& position, const Vec3& half_extent, f32 density,
                                   const Vec3& initial_velocity) {
  if (!impl_) return 0;
  JPH::Ref<JPH::BoxShape> shape = new JPH::BoxShape(ToJolt(half_extent));
  shape->SetDensity(density);
  JPH::BodyCreationSettings settings(shape, ToJolt(position), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Dynamic, layers::kDynamic);
  settings.mLinearVelocity = ToJolt(initial_velocity);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  impl_->dynamic_bodies.push_back(id);
  ++dynamic_count_;
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddDynamicSphere(const Vec3& position, f32 radius, f32 density,
                                      const Vec3& initial_velocity) {
  if (!impl_) return 0;
  JPH::Ref<JPH::SphereShape> shape = new JPH::SphereShape(radius);
  shape->SetDensity(density);
  JPH::BodyCreationSettings settings(shape, ToJolt(position), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Dynamic, layers::kDynamic);
  settings.mLinearVelocity = ToJolt(initial_velocity);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  impl_->dynamic_bodies.push_back(id);
  ++dynamic_count_;
  return id.GetIndexAndSequenceNumber() + 1;
}

CharacterId PhysicsWorld::CreateCharacter(const Vec3& position, f32 radius, f32 half_height) {
  if (!impl_) return 0;
  JPH::Ref<JPH::CharacterVirtualSettings> settings = new JPH::CharacterVirtualSettings();
  settings->mShape = new JPH::CapsuleShape(half_height, radius);
  // Accept ground contacts on the lower hemisphere so slopes register.
  settings->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);
  JPH::Ref<JPH::CharacterVirtual> character = new JPH::CharacterVirtual(
      settings, ToJolt(position), JPH::Quat::sIdentity(), 0, impl_->system.get());
  impl_->characters.push_back({character, 0.0f});
  return impl_->characters.size();  // id = index + 1
}

void PhysicsWorld::MoveCharacter(CharacterId id, const Vec3& horizontal_velocity, bool jump,
                                 f32 dt, Vec3* out_position, bool* out_grounded) {
  if (!impl_ || id == 0 || id > impl_->characters.size()) return;
  Impl::CharacterEntry& entry = impl_->characters[id - 1];
  JPH::CharacterVirtual* character = entry.character;

  bool grounded = character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
  if (grounded && entry.vy < 0) entry.vy = 0;
  if (grounded && jump) entry.vy = 4.5f;
  entry.vy += impl_->system->GetGravity().GetY() * dt;

  character->SetLinearVelocity({horizontal_velocity.x, entry.vy, horizontal_velocity.z});
  JPH::CharacterVirtual::ExtendedUpdateSettings update;
  update.mWalkStairsStepUp = JPH::Vec3(0, entry.step_height, 0);
  character->ExtendedUpdate(dt, impl_->system->GetGravity(), update,
                            impl_->system->GetDefaultBroadPhaseLayerFilter(layers::kDynamic),
                            impl_->system->GetDefaultLayerFilter(layers::kDynamic), {}, {},
                            *impl_->temp_allocator);
  JPH::RVec3 p = character->GetPosition();
  if (out_position) {
    *out_position = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
                     static_cast<f32>(p.GetZ())};
  }
  if (out_grounded) {
    *out_grounded = character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
  }
}

void PhysicsWorld::MoveCharacterVelocity(CharacterId id, const Vec3& velocity, f32 dt,
                                         Vec3* out_position, bool* out_grounded,
                                         Vec3* out_ground_velocity) {
  if (!impl_ || id == 0 || id > impl_->characters.size()) return;
  Impl::CharacterEntry& entry = impl_->characters[id - 1];
  JPH::CharacterVirtual* character = entry.character;

  character->SetLinearVelocity(ToJolt(velocity));
  JPH::CharacterVirtual::ExtendedUpdateSettings update;
  update.mWalkStairsStepUp = JPH::Vec3(0, entry.step_height, 0);
  character->ExtendedUpdate(dt, impl_->system->GetGravity(), update,
                            impl_->system->GetDefaultBroadPhaseLayerFilter(layers::kDynamic),
                            impl_->system->GetDefaultLayerFilter(layers::kDynamic), {}, {},
                            *impl_->temp_allocator);
  JPH::RVec3 p = character->GetPosition();
  if (out_position) {
    *out_position = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
                     static_cast<f32>(p.GetZ())};
  }
  const bool grounded =
      character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
  if (out_grounded) *out_grounded = grounded;
  if (out_ground_velocity) {
    *out_ground_velocity = Vec3{};
    if (grounded) {
      character->UpdateGroundVelocity();
      const JPH::Vec3 gv = character->GetGroundVelocity();
      *out_ground_velocity = {gv.GetX(), gv.GetY(), gv.GetZ()};
    }
  }
}

void PhysicsWorld::SetCharacterPosition(CharacterId id, const Vec3& position) {
  if (!impl_ || id == 0 || id > impl_->characters.size()) return;
  impl_->characters[id - 1].character->SetPosition(ToJolt(position));
}

bool PhysicsWorld::GetCharacterPosition(CharacterId id, Vec3* out_position) const {
  if (!impl_ || id == 0 || id > impl_->characters.size()) return false;
  JPH::RVec3 p = impl_->characters[id - 1].character->GetPosition();
  if (out_position) {
    *out_position = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
                     static_cast<f32>(p.GetZ())};
  }
  return true;
}

void PhysicsWorld::ConfigureCharacter(CharacterId id, f32 max_slope_angle, f32 step_height) {
  if (!impl_ || id == 0 || id > impl_->characters.size()) return;
  Impl::CharacterEntry& entry = impl_->characters[id - 1];
  entry.character->SetMaxSlopeAngle(max_slope_angle);
  entry.step_height = std::max(step_height, 0.0f);
}

bool PhysicsWorld::SetCharacterShape(CharacterId id, f32 radius, f32 half_height) {
  if (!impl_ || id == 0 || id > impl_->characters.size()) return false;
  JPH::CharacterVirtual* character = impl_->characters[id - 1].character;
  JPH::Ref<JPH::CapsuleShape> shape = new JPH::CapsuleShape(std::max(half_height, 0.0f), radius);
  // A small penetration tolerance keeps a graze from vetoing an otherwise-clear
  // stand-up; a genuinely blocked capsule overlaps far deeper than this.
  return character->SetShape(
      shape, 0.05f, impl_->system->GetDefaultBroadPhaseLayerFilter(layers::kDynamic),
      impl_->system->GetDefaultLayerFilter(layers::kDynamic), {}, {}, *impl_->temp_allocator);
}

bool PhysicsWorld::SphereCast(const Vec3& origin, const Vec3& direction, f32 max_distance,
                              f32 radius, RayHit* out) const {
  if (!impl_) return false;
  Vec3 dir = Normalize(direction);
  JPH::Ref<JPH::SphereShape> sphere = new JPH::SphereShape(std::max(radius, 1e-3f));
  JPH::RShapeCast shape_cast(sphere, JPH::Vec3::sReplicate(1.0f),
                             JPH::RMat44::sTranslation(ToJolt(origin)),
                             ToJolt(dir) * max_distance);
  JPH::ShapeCastSettings settings;
  // Treat back-facing triangles as solid so a sphere starting just inside a wall
  // still reports the near surface (camera pulls forward reliably).
  settings.mBackFaceModeTriangles = JPH::EBackFaceMode::CollideWithBackFaces;
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
  impl_->system->GetNarrowPhaseQuery().CastShape(shape_cast, settings, JPH::RVec3::sZero(),
                                                 collector);
  if (!collector.HadHit()) return false;
  const f32 distance = collector.mHit.mFraction * max_distance;
  out->distance = distance;
  out->position = {origin.x + dir.x * distance, origin.y + dir.y * distance,
                   origin.z + dir.z * distance};
  const JPH::Vec3 n = -collector.mHit.mPenetrationAxis.Normalized();
  out->normal = {n.GetX(), n.GetY(), n.GetZ()};
  return true;
}

BodyId PhysicsWorld::AddKinematicCapsule(const Vec3& position, f32 radius, f32 half_height) {
  if (!impl_) return 0;
  JPH::Ref<JPH::CapsuleShape> shape = new JPH::CapsuleShape(half_height, radius);
  // Kinematic (driven by SetBodyPosition, immune to gravity/forces) but in the
  // dynamic layer so the player's character controller collides with it.
  JPH::BodyCreationSettings settings(shape, ToJolt(position), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Kinematic, layers::kDynamic);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddKinematicBox(const Vec3& position, const Vec3& half_extent) {
  if (!impl_) return 0;
  JPH::Ref<JPH::BoxShape> shape = new JPH::BoxShape(ToJolt(half_extent));
  // Kinematic but in the dynamic layer so character controllers collide with
  // it (same scheme as AddKinematicCapsule).
  JPH::BodyCreationSettings settings(shape, ToJolt(position), JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Kinematic, layers::kDynamic);
  JPH::BodyID id =
      impl_->system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
  return id.GetIndexAndSequenceNumber() + 1;
}

void PhysicsWorld::MoveBodyKinematic(BodyId id, const Vec3& position, const f32 rotation[4],
                                     f32 dt) {
  if (!impl_ || id == 0) return;
  if (dt <= 0) {
    SetBodyPosition(id, position, rotation);
    return;
  }
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  JPH::Quat q(rotation[0], rotation[1], rotation[2], rotation[3]);
  impl_->system->GetBodyInterface().MoveKinematic(body, ToJolt(position), q, dt);
}

void PhysicsWorld::SetBodyPosition(BodyId id, const Vec3& position, const f32 rotation[4]) {
  if (!impl_ || id == 0) return;
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  JPH::Quat q(rotation[0], rotation[1], rotation[2], rotation[3]);
  impl_->system->GetBodyInterface().SetPositionAndRotation(body, ToJolt(position), q,
                                                           JPH::EActivation::Activate);
}

void PhysicsWorld::RemoveBody(BodyId id) {
  if (!impl_ || id == 0) return;
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  impl_->body_surface.erase(body.GetIndexAndSequenceNumber());
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  bodies.RemoveBody(body);
  bodies.DestroyBody(body);
  for (size_t i = 0; i < impl_->dynamic_bodies.size(); ++i) {
    if (impl_->dynamic_bodies[i] == body) {
      impl_->dynamic_bodies[i] = impl_->dynamic_bodies.back();
      impl_->dynamic_bodies.pop_back();
      --dynamic_count_;
      break;
    }
  }
}

VehicleId PhysicsWorld::CreateVehicle(const VehicleDesc& desc, const Vec3& position,
                                      f32 yaw_radians) {
  if (!impl_) return 0;

  // Chassis: a box with the center of mass dropped for arcade stability.
  JPH::RefConst<JPH::Shape> box = new JPH::BoxShape(ToJolt(desc.half_extent));
  JPH::ShapeSettings::ShapeResult chassis_result =
      JPH::OffsetCenterOfMassShapeSettings(JPH::Vec3(0, -desc.com_drop, desc.com_fore),
                                           new JPH::BoxShapeSettings(ToJolt(desc.half_extent)))
          .Create();
  if (chassis_result.HasError()) {
    RX_WARN("vehicle chassis shape failed: {}", chassis_result.GetError().c_str());
    return 0;
  }

  JPH::BodyCreationSettings body_settings(
      chassis_result.Get(), ToJolt(position),
      JPH::Quat::sRotation(JPH::Vec3::sAxisY(), yaw_radians), JPH::EMotionType::Dynamic,
      layers::kDynamic);
  body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  body_settings.mMassPropertiesOverride.mMass = desc.mass;
  // Cars scrape walls constantly; a little friction keeps glancing contact
  // from killing all speed, and high angular damping settles landings.
  body_settings.mFriction = 0.4f;
  body_settings.mAngularDamping = 0.6f;

  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  JPH::Body* body = bodies.CreateBody(body_settings);
  if (!body) return 0;
  bodies.AddBody(body->GetID(), JPH::EActivation::Activate);
  impl_->dynamic_bodies.push_back(body->GetID());

  JPH::VehicleConstraintSettings vehicle;
  vehicle.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);

  const f32 attach_y = -0.9f * desc.half_extent.y;
  auto make_wheel = [&](f32 x, f32 z, bool front) {
    JPH::WheelSettingsWV* w = new JPH::WheelSettingsWV;
    w->mPosition = JPH::Vec3(x, attach_y, z);
    w->mRadius = desc.wheel_radius;
    w->mWidth = desc.wheel_width;
    w->mSuspensionMinLength = desc.suspension_min;
    w->mSuspensionMaxLength = desc.suspension_max;
    w->mMaxSteerAngle = front ? desc.max_steer_angle : 0.0f;
    // Handbrake locks the rear axle only: the drift lever.
    const f32 handbrake = desc.max_handbrake_torque < 0 ? 8000.0f : desc.max_handbrake_torque;
    w->mMaxHandBrakeTorque = front ? 0.0f : handbrake;
    // Brake bias: split the per-wheel base brake torque front/rear. At the even
    // 0.5 default with no explicit base this is the untouched Jolt default; any
    // bias or an explicit base sets each wheel to base * 2 * (its axle share).
    if (desc.max_brake_torque > 0 || desc.brake_bias_front != 0.5f) {
      const f32 base = desc.max_brake_torque > 0 ? desc.max_brake_torque : 1500.0f;
      const f32 share = front ? desc.brake_bias_front : (1.0f - desc.brake_bias_front);
      w->mMaxBrakeTorque = base * 2.0f * std::clamp(share, 0.0f, 1.0f);
    }
    // Suspension spring: per-axle frequency (falls back to the shared value,
    // then Jolt's default); shared damping.
    const f32 axle_freq = front ? desc.front_suspension_frequency : desc.rear_suspension_frequency;
    const f32 freq = axle_freq > 0 ? axle_freq : desc.suspension_frequency;
    if (freq > 0) w->mSuspensionSpring.mFrequency = freq;
    if (desc.suspension_damping > 0) w->mSuspensionSpring.mDamping = desc.suspension_damping;
    // Tire grip: scale Jolt's default slip curves. Longitudinal is shared;
    // lateral takes a per-axle scalar (fall back to the shared one, then 1) so a
    // profile can bias the balance toward under- or oversteer.
    if (desc.tire_long_friction > 0) {
      for (JPH::LinearCurve::Point& p : w->mLongitudinalFriction.mPoints)
        p.mY *= desc.tire_long_friction;
    }
    const f32 axle_lat = front ? desc.front_lat_friction : desc.rear_lat_friction;
    const f32 lat = axle_lat > 0 ? axle_lat : desc.tire_lat_friction;
    if (lat > 0) {
      for (JPH::LinearCurve::Point& p : w->mLateralFriction.mPoints) p.mY *= lat;
    }
    return w;
  };
  vehicle.mWheels = {
      make_wheel(-desc.wheel_x, desc.front_z, true),   // FL
      make_wheel(desc.wheel_x, desc.front_z, true),    // FR
      make_wheel(-desc.wheel_x, desc.rear_z, false),   // RL
      make_wheel(desc.wheel_x, desc.rear_z, false),    // RR
  };

  JPH::WheeledVehicleControllerSettings* controller = new JPH::WheeledVehicleControllerSettings;
  controller->mEngine.mMaxTorque = desc.max_engine_torque;
  if (desc.max_rpm > 0) controller->mEngine.mMaxRPM = desc.max_rpm;
  if (desc.min_rpm > 0) controller->mEngine.mMinRPM = desc.min_rpm;
  if (desc.engine_inertia > 0) controller->mEngine.mInertia = desc.engine_inertia;
  if (desc.engine_braking > 0) controller->mEngine.mAngularDamping = desc.engine_braking;
  ApplyTorqueCurve(controller->mEngine, desc.torque_curve, desc.torque_curve_count);
  controller->mTransmission.mClutchStrength =
      desc.clutch_strength > 0 ? desc.clutch_strength : 10.0f;
  if (desc.gear_count > 0) {
    controller->mTransmission.mGearRatios.clear();
    for (u32 g = 0; g < desc.gear_count && g < 8; ++g)
      controller->mTransmission.mGearRatios.push_back(desc.gear_ratios[g]);
  }
  if (desc.shift_up_rpm > 0) controller->mTransmission.mShiftUpRPM = desc.shift_up_rpm;
  if (desc.shift_down_rpm > 0) controller->mTransmission.mShiftDownRPM = desc.shift_down_rpm;

  // Driven axles by drivetrain; limited slip differentials throughout (the
  // Jolt default 1.4 ratio) - RWD gives throttle oversteer, AWD splits by
  // awd_front_split. A free-rolling chassis has no differential at all, so no
  // engine torque reaches any wheel and all four wheels coast on their
  // suspension and tire friction (a towed trailer / carriage); steering and
  // the handbrake still work per wheel.
  const f32 final_drive = desc.final_drive > 0 ? desc.final_drive : 3.42f;
  // A free-rolling chassis keeps an empty differential list, so the engine is
  // disconnected from every wheel and they coast on suspension + tire friction.
  if (!desc.free_rolling) {
    switch (desc.drivetrain) {
      case Drivetrain::kRWD:
        controller->mDifferentials.resize(1);
        controller->mDifferentials[0].mLeftWheel = 2;
        controller->mDifferentials[0].mRightWheel = 3;
        controller->mDifferentials[0].mDifferentialRatio = final_drive;
        break;
      case Drivetrain::kFWD:
        controller->mDifferentials.resize(1);
        controller->mDifferentials[0].mLeftWheel = 0;
        controller->mDifferentials[0].mRightWheel = 1;
        controller->mDifferentials[0].mDifferentialRatio = final_drive;
        break;
      case Drivetrain::kAWD:
        controller->mDifferentials.resize(2);
        controller->mDifferentials[0].mLeftWheel = 0;
        controller->mDifferentials[0].mRightWheel = 1;
        controller->mDifferentials[0].mDifferentialRatio = final_drive;
        controller->mDifferentials[0].mEngineTorqueRatio = desc.awd_front_split;
        controller->mDifferentials[1].mLeftWheel = 2;
        controller->mDifferentials[1].mRightWheel = 3;
        controller->mDifferentials[1].mDifferentialRatio = final_drive;
        controller->mDifferentials[1].mEngineTorqueRatio = 1.0f - desc.awd_front_split;
        break;
    }
  }
  // Limited-slip differential: tighten (or open) every driven differential.
  if (desc.limited_slip_ratio > 0) {
    for (JPH::VehicleDifferentialSettings& diff : controller->mDifferentials)
      diff.mLimitedSlipRatio = desc.limited_slip_ratio;
  }
  vehicle.mController = controller;

  // Anti-roll bars per axle, like the Jolt vehicle sample. Front and rear
  // stiffness are independent; each falls back to the shared anti_roll_stiffness
  // then to Jolt's default, so a profile can bias roll stiffness fore/aft.
  vehicle.mAntiRollBars.resize(2);
  vehicle.mAntiRollBars[0].mLeftWheel = 0;
  vehicle.mAntiRollBars[0].mRightWheel = 1;
  vehicle.mAntiRollBars[1].mLeftWheel = 2;
  vehicle.mAntiRollBars[1].mRightWheel = 3;
  const f32 arb_front = desc.anti_roll_front > 0 ? desc.anti_roll_front : desc.anti_roll_stiffness;
  const f32 arb_rear = desc.anti_roll_rear > 0 ? desc.anti_roll_rear : desc.anti_roll_stiffness;
  if (arb_front > 0) vehicle.mAntiRollBars[0].mStiffness = arb_front;
  if (arb_rear > 0) vehicle.mAntiRollBars[1].mStiffness = arb_rear;

  JPH::Ref<JPH::VehicleConstraint> constraint = new JPH::VehicleConstraint(*body, vehicle);
  constraint->SetVehicleCollisionTester(
      new JPH::VehicleCollisionTesterCastCylinder(layers::kDynamic, 0.05f));
  impl_->system->AddConstraint(constraint);
  impl_->system->AddStepListener(constraint);

  impl_->vehicles.push_back({body->GetID(), constraint, true, 4, desc.downforce});
  Impl::VehicleEntry& entry = impl_->vehicles.back();
  entry.downforce_balance = desc.downforce_balance;
  entry.front_z = desc.front_z;
  entry.rear_z = desc.rear_z;
  entry.steer_high_speed_fraction = desc.steer_high_speed_fraction;
  entry.steer_fade_speed = desc.steer_fade_speed;
  entry.traction_control = desc.traction_control;
  InstallVehicleFriction(static_cast<u32>(impl_->vehicles.size() - 1));
  return impl_->vehicles.size();
}

VehicleId PhysicsWorld::CreateMotorcycle(const MotorcycleDesc& desc, const Vec3& position,
                                         f32 yaw_radians) {
  if (!impl_) return 0;

  // Chassis, the Jolt MotorcycleTest scheme: narrow box with the center of
  // mass dropped (rider crouch), full inertia otherwise.
  JPH::ShapeSettings::ShapeResult chassis_result =
      JPH::OffsetCenterOfMassShapeSettings(JPH::Vec3(0, -desc.com_drop, 0),
                                           new JPH::BoxShapeSettings(ToJolt(desc.half_extent)))
          .Create();
  if (chassis_result.HasError()) {
    RX_WARN("motorcycle chassis shape failed: {}", chassis_result.GetError().c_str());
    return 0;
  }

  JPH::BodyCreationSettings body_settings(
      chassis_result.Get(), ToJolt(position),
      JPH::Quat::sRotation(JPH::Vec3::sAxisY(), yaw_radians), JPH::EMotionType::Dynamic,
      layers::kDynamic);
  body_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  body_settings.mMassPropertiesOverride.mMass = desc.mass;
  body_settings.mFriction = 0.4f;

  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  JPH::Body* body = bodies.CreateBody(body_settings);
  if (!body) return 0;
  bodies.AddBody(body->GetID(), JPH::EActivation::Activate);
  impl_->dynamic_bodies.push_back(body->GetID());

  JPH::VehicleConstraintSettings vehicle;
  vehicle.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);

  const f32 attach_y = -0.9f * desc.half_extent.y;

  JPH::WheelSettingsWV* front = new JPH::WheelSettingsWV;
  front->mPosition = JPH::Vec3(0, attach_y, desc.front_z);
  front->mMaxSteerAngle = desc.max_steer_angle;
  // Caster: the fork rakes back, and the steering axis follows the fork.
  front->mSuspensionDirection = JPH::Vec3(0, -1, std::tan(desc.caster_angle)).Normalized();
  front->mSteeringAxis = -front->mSuspensionDirection;
  front->mRadius = desc.wheel_radius;
  front->mWidth = desc.wheel_width;
  front->mSuspensionMinLength = desc.suspension_min;
  front->mSuspensionMaxLength = desc.suspension_max;
  front->mSuspensionSpring.mFrequency = desc.front_suspension_frequency;
  front->mMaxBrakeTorque = desc.front_brake_torque;
  front->mMaxHandBrakeTorque = 0;

  JPH::WheelSettingsWV* rear = new JPH::WheelSettingsWV;
  rear->mPosition = JPH::Vec3(0, attach_y, desc.rear_z);
  rear->mMaxSteerAngle = 0;
  rear->mRadius = desc.wheel_radius;
  rear->mWidth = desc.wheel_width;
  rear->mSuspensionMinLength = desc.suspension_min;
  rear->mSuspensionMaxLength = desc.suspension_max;
  rear->mSuspensionSpring.mFrequency = desc.rear_suspension_frequency;
  rear->mMaxBrakeTorque = desc.rear_brake_torque;
  rear->mMaxHandBrakeTorque = 0;

  for (JPH::WheelSettingsWV* w : {front, rear}) {
    if (desc.tire_long_friction > 0) {
      for (JPH::LinearCurve::Point& p : w->mLongitudinalFriction.mPoints)
        p.mY *= desc.tire_long_friction;
    }
    if (desc.tire_lat_friction > 0) {
      for (JPH::LinearCurve::Point& p : w->mLateralFriction.mPoints) p.mY *= desc.tire_lat_friction;
    }
  }

  vehicle.mWheels = {front, rear};

  JPH::MotorcycleControllerSettings* controller = new JPH::MotorcycleControllerSettings;
  controller->mEngine.mMaxTorque = desc.max_engine_torque;
  controller->mEngine.mMaxRPM = desc.max_rpm;
  controller->mEngine.mMinRPM = desc.min_rpm;
  if (desc.engine_inertia > 0) controller->mEngine.mInertia = desc.engine_inertia;
  if (desc.engine_braking > 0) controller->mEngine.mAngularDamping = desc.engine_braking;
  ApplyTorqueCurve(controller->mEngine, desc.torque_curve, desc.torque_curve_count);
  controller->mTransmission.mShiftUpRPM = desc.shift_up_rpm;
  controller->mTransmission.mShiftDownRPM = desc.shift_down_rpm;
  controller->mTransmission.mClutchStrength = 2.0f;
  if (desc.gear_count > 0) {
    controller->mTransmission.mGearRatios.clear();
    for (u32 g = 0; g < desc.gear_count && g < 8; ++g)
      controller->mTransmission.mGearRatios.push_back(desc.gear_ratios[g]);
  } else {
    controller->mTransmission.mGearRatios = {2.27f, 1.63f, 1.3f, 1.09f, 0.96f, 0.88f};
  }
  controller->mTransmission.mReverseGearRatios = {-4.0f};
  controller->mMaxLeanAngle = desc.max_lean_angle;
  controller->mLeanSpringConstant = desc.lean_spring;
  controller->mLeanSpringDamping = desc.lean_damping;
  // A motorcycle still needs one differential to route engine torque; the
  // rear wheel is the drive wheel.
  controller->mDifferentials.resize(1);
  controller->mDifferentials[0].mLeftWheel = -1;
  controller->mDifferentials[0].mRightWheel = 1;
  controller->mDifferentials[0].mDifferentialRatio = desc.final_drive;
  vehicle.mController = controller;

  JPH::Ref<JPH::VehicleConstraint> constraint = new JPH::VehicleConstraint(*body, vehicle);
  // Full-width convex radius: a rounded tire profile so leaning over the
  // edge of the contact patch stays smooth (the Jolt sample scheme).
  constraint->SetVehicleCollisionTester(
      new JPH::VehicleCollisionTesterCastCylinder(layers::kDynamic, 1.0f));
  impl_->system->AddConstraint(constraint);
  impl_->system->AddStepListener(constraint);

  impl_->vehicles.push_back({body->GetID(), constraint, true, 2, desc.downforce});
  impl_->vehicles.back().traction_control = desc.traction_control;
  InstallVehicleFriction(static_cast<u32>(impl_->vehicles.size() - 1));
  return impl_->vehicles.size();
}

void PhysicsWorld::RemoveVehicle(VehicleId id) {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return;
  Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return;
  impl_->system->RemoveStepListener(entry.constraint);
  impl_->system->RemoveConstraint(entry.constraint);
  entry.constraint = nullptr;
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  bodies.RemoveBody(entry.body);
  bodies.DestroyBody(entry.body);
  for (size_t i = 0; i < impl_->dynamic_bodies.size(); ++i) {
    if (impl_->dynamic_bodies[i] == entry.body) {
      impl_->dynamic_bodies.erase(impl_->dynamic_bodies.begin() + i);
      break;
    }
  }
  entry.alive = false;
}

void PhysicsWorld::InstallVehicleFriction(u32 vehicle_index) {
  if (!impl_ || vehicle_index >= impl_->vehicles.size()) return;
  JPH::VehicleConstraint* constraint = impl_->vehicles[vehicle_index].constraint.GetPtr();
  if (!constraint) return;
  // Surface-aware tire friction: instead of Jolt's default sqrt(tire, body)
  // combine, scale the tire's own longitudinal/lateral grip by the contact
  // surface's grip table, global rain wetness, and per-wheel aquaplaning. This
  // supersedes the ground body's mFriction for tire contacts.
  constraint->SetCombineFriction([this, vehicle_index](JPH::uint wheel_index, float& io_long,
                                                       float& io_lat, const JPH::Body& body2,
                                                       const JPH::SubShapeID& sub) {
    // Resolve the surface: the contact shape's material, else the per-body side
    // map (shared-shape instances), else asphalt.
    SurfaceType surf = impl_->surface_of(body2.GetShape()->GetMaterial(sub));
    if (surf == SurfaceType::kAsphalt) {
      if (const SurfaceType* mapped =
              impl_->body_surface.find(body2.GetID().GetIndexAndSequenceNumber())) {
        surf = *mapped;
      }
    }
    const SurfaceGripEntry& g = kSurfaceGrip[static_cast<u32>(surf)];
    const f32 wet = WetGripMultiplier(surf, surface_wetness_);
    f32 grip_long = g.longitudinal * wet;
    f32 grip_lat = g.lateral * wet;

    const Impl::VehicleEntry& entry = impl_->vehicles[vehicle_index];
    const auto* wheel = static_cast<const JPH::WheelWV*>(entry.constraint->GetWheel(wheel_index));
    if (water_height_ && wheel->HasContact()) {
      const JPH::RVec3 cp = wheel->GetContactPosition();
      const Vec3 p{static_cast<f32>(cp.GetX()), static_cast<f32>(cp.GetY()),
                   static_cast<f32>(cp.GetZ())};
      f32 surface_h = 0;
      if (SampleWater(p, &surface_h, nullptr) && p.y < surface_h) {
        const f32 aqua =
            AquaplaneGrip(surface_h - p.y, wheel->GetSettings()->mRadius, entry.cached_speed);
        grip_long *= aqua;
        grip_lat *= aqua;
      }
    }
    io_long *= grip_long;
    io_lat *= grip_lat;
  });
}

void PhysicsWorld::set_surface_wetness(f32 wetness) {
  surface_wetness_ = std::clamp(wetness, 0.0f, 1.0f);
}

void PhysicsWorld::SetManualTransmission(VehicleId id, bool manual) {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return;
  Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return;
  entry.manual = manual;
  auto* controller = static_cast<JPH::WheeledVehicleController*>(entry.constraint->GetController());
  JPH::VehicleTransmission& trans = controller->GetTransmission();
  trans.mMode = manual ? JPH::ETransmissionMode::Manual : JPH::ETransmissionMode::Auto;
  // Manual mode won't auto-select a gear; drop it into 1st (clutch engaged) so
  // the first throttle drives, matching how you'd start off in a manual car.
  if (manual && trans.GetCurrentGear() == 0) trans.Set(1, 1.0f);
}

void PhysicsWorld::DriveVehicle(VehicleId id, const VehicleInput& input) {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return;
  Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return;
  if (!entry.manual) {
    // Automatic box: reuse the 4-float path (traction control, auto clutch);
    // clutch and shift edges are ignored.
    DriveVehicle(id, input.throttle, input.steer, input.brake, input.handbrake);
    return;
  }
  auto* controller = static_cast<JPH::WheeledVehicleController*>(entry.constraint->GetController());
  JPH::VehicleTransmission& trans = controller->GetTransmission();
  int gear = trans.GetCurrentGear();
  const int max_forward = static_cast<int>(trans.mGearRatios.size());
  const int min_gear = -static_cast<int>(trans.mReverseGearRatios.size());
  if (input.shift_up && !entry.prev_shift_up && gear < max_forward) ++gear;
  if (input.shift_down && !entry.prev_shift_down && gear > min_gear) --gear;
  entry.prev_shift_up = input.shift_up;
  entry.prev_shift_down = input.shift_down;
  // Clutch: 1 = fully disengaged -> 0 friction, so the engine spins free of the
  // wheels. Applied every step so the pedal tracks continuously between shifts.
  trans.Set(gear, std::clamp(1.0f - input.clutch, 0.0f, 1.0f));
  controller->SetDriverInput(
      input.throttle,
      input.steer * SteerFadeScale(entry.steer_high_speed_fraction, entry.steer_fade_speed,
                                   entry.cached_speed),
      input.brake, input.handbrake);
  if (input.throttle != 0 || input.steer != 0 || input.brake != 0 || input.handbrake != 0) {
    impl_->system->GetBodyInterface().ActivateBody(entry.body);
  }
}

void PhysicsWorld::DriveVehicle(VehicleId id, f32 forward, f32 right, f32 brake, f32 handbrake) {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return;
  Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return;
  auto* controller = static_cast<JPH::WheeledVehicleController*>(entry.constraint->GetController());
  right *= SteerFadeScale(entry.steer_high_speed_fraction, entry.steer_fade_speed,
                          entry.cached_speed);
  // Traction control: govern the throttle toward ~8% longitudinal slip on
  // the contact wheels, keeping the tire near its friction peak instead of
  // lighting it up (and unblocking Jolt's slip-gated automatic upshift).
  // Disengaged below ~5 m/s like a real TC: the slip ratio's |v| denominator
  // makes launch slip read enormous, and killing the launch is worse than a
  // little wheelspin off the line.
  if (entry.traction_control && forward != 0) {
    JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
    const JPH::Vec3 velocity = bodies.GetLinearVelocity(entry.body);
    const JPH::Vec3 fwd_axis = bodies.GetRotation(entry.body) * JPH::Vec3::sAxisZ();
    if (std::fabs(velocity.Dot(fwd_axis)) > 5.0f) {
      f32 max_slip = 0;
      for (u32 i = 0; i < entry.wheel_count; ++i) {
        const auto* wheel = static_cast<const JPH::WheelWV*>(entry.constraint->GetWheel(i));
        if (wheel->HasContact()) max_slip = std::max(max_slip, wheel->mLongitudinalSlip);
      }
      // Proportional tracker: aim the throttle at the fraction that would put
      // the worst wheel on the slip target, smoothed so cut and recovery move
      // at the same rate (a ratcheting governor parks at its floor).
      constexpr f32 kSlipTarget = 0.09f;
      const f32 target =
          max_slip > kSlipTarget ? std::max(0.12f, kSlipTarget / max_slip) : 1.0f;
      entry.tc_scale += (target - entry.tc_scale) * 0.25f;
      forward *= std::min(1.0f, std::max(0.12f, entry.tc_scale));
    } else {
      entry.tc_scale = 1;
    }
  }
  controller->SetDriverInput(forward, right, brake, handbrake);
  if (forward != 0 || right != 0 || brake != 0 || handbrake != 0) {
    impl_->system->GetBodyInterface().ActivateBody(entry.body);
  }
}

bool PhysicsWorld::GetVehicleTransform(VehicleId id, Vec3* position, f32 rotation[4]) const {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return false;
  const Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return false;
  JPH::RVec3 p;
  JPH::Quat q;
  impl_->system->GetBodyInterface().GetPositionAndRotation(entry.body, p, q);
  *position = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
               static_cast<f32>(p.GetZ())};
  rotation[0] = q.GetX();
  rotation[1] = q.GetY();
  rotation[2] = q.GetZ();
  rotation[3] = q.GetW();
  return true;
}

bool PhysicsWorld::GetVehicleWheel(VehicleId id, u32 wheel, Vec3* position,
                                   f32 rotation[4]) const {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return false;
  const Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive || wheel >= entry.wheel_count) return false;
  const JPH::RMat44 m =
      entry.constraint->GetWheelWorldTransform(wheel, JPH::Vec3::sAxisX(), JPH::Vec3::sAxisY());
  const JPH::RVec3 p = m.GetTranslation();
  const JPH::Quat q = m.GetRotation().GetQuaternion().Normalized();
  *position = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
               static_cast<f32>(p.GetZ())};
  rotation[0] = q.GetX();
  rotation[1] = q.GetY();
  rotation[2] = q.GetZ();
  rotation[3] = q.GetW();
  return true;
}

f32 PhysicsWorld::VehicleForwardSpeed(VehicleId id) const {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return 0;
  const Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return 0;
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  const JPH::Vec3 velocity = bodies.GetLinearVelocity(entry.body);
  const JPH::Vec3 forward = bodies.GetRotation(entry.body) * JPH::Vec3::sAxisZ();
  return velocity.Dot(forward);
}

BodyId PhysicsWorld::GetVehicleBody(VehicleId id) const {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return 0;
  const Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return 0;
  return entry.body.GetIndexAndSequenceNumber() + 1;
}

bool PhysicsWorld::GetVehicleState(VehicleId id, VehicleState* out) const {
  if (!impl_ || id == 0 || id > impl_->vehicles.size() || !out) return false;
  const Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return false;

  const auto* controller =
      static_cast<const JPH::WheeledVehicleController*>(entry.constraint->GetController());
  const JPH::VehicleEngine& engine = controller->GetEngine();
  const JPH::VehicleTransmission& trans = controller->GetTransmission();
  out->rpm = engine.GetCurrentRPM();
  out->gear = trans.GetCurrentGear();
  out->forward_speed = VehicleForwardSpeed(id);
  // Engine load: the throttle the box actually lets reach the engine (auto mode
  // multiplies by clutch friction, matching WheeledVehicleController). Since
  // GetTorque is linear in that input, load == delivered/max at this rpm.
  const f32 clutch = trans.GetClutchFriction();
  const f32 applied = std::fabs(controller->GetForwardInput()) *
                      (trans.mMode == JPH::ETransmissionMode::Auto ? clutch : 1.0f);
  const f32 max_torque_at_rpm = engine.GetTorque(1.0f);
  out->engine_torque = engine.GetTorque(applied);
  out->engine_load =
      max_torque_at_rpm > 1.0e-3f ? std::clamp(out->engine_torque / max_torque_at_rpm, 0.0f, 1.0f)
                                  : 0.0f;
  out->is_shifting = clutch < 0.99f;
  out->wheel_count = entry.wheel_count;

  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  for (u32 i = 0; i < entry.wheel_count && i < 4; ++i) {
    const auto* wheel = static_cast<const JPH::WheelWV*>(entry.constraint->GetWheel(i));
    const JPH::WheelSettings* settings = wheel->GetSettings();
    VehicleState::WheelState& ws = out->wheels[i];
    ws.contact = wheel->HasContact();
    ws.suspension_length = wheel->GetSuspensionLength();
    const f32 travel = settings->mSuspensionMaxLength - settings->mSuspensionMinLength;
    ws.suspension_compression =
        travel > 1.0e-4f
            ? std::clamp((settings->mSuspensionMaxLength - wheel->GetSuspensionLength()) / travel,
                         0.0f, 1.0f)
            : 0.0f;
    ws.longitudinal_slip = wheel->mLongitudinalSlip;
    ws.lateral_slip = wheel->mLateralSlip;
    ws.angular_velocity = wheel->GetAngularVelocity();
    ws.rotation_angle = wheel->GetRotationAngle();
    ws.surface = SurfaceType::kAsphalt;
    ws.submerged = false;
    ws.wading_depth = 0;
    if (wheel->HasContact()) {
      if (JPH::RefConst<JPH::Shape> shape = bodies.GetShape(wheel->GetContactBodyID())) {
        ws.surface = impl_->surface_of(shape->GetMaterial(wheel->GetContactSubShapeID()));
      }
      if (ws.surface == SurfaceType::kAsphalt) {
        if (const SurfaceType* mapped = impl_->body_surface.find(
                wheel->GetContactBodyID().GetIndexAndSequenceNumber())) {
          ws.surface = *mapped;
        }
      }
      if (water_height_) {
        const JPH::RVec3 cp = wheel->GetContactPosition();
        const Vec3 p{static_cast<f32>(cp.GetX()), static_cast<f32>(cp.GetY()),
                     static_cast<f32>(cp.GetZ())};
        f32 surface_h = 0;
        if (SampleWater(p, &surface_h, nullptr) && p.y < surface_h) {
          ws.submerged = true;
          ws.wading_depth = surface_h - p.y;
        }
      }
    }
  }
  return true;
}

StrandGroomId PhysicsWorld::CreateStrandGroom(const StrandGroomDesc& desc,
                                              const Mat4& transform) {
  if (!impl_ || !desc.points || desc.strand_count == 0 || desc.points_per_strand < 2) return 0;
  const u32 pps = desc.points_per_strand;
  const u32 node_count = desc.strand_count * pps;
  const Vec3 origin = Translation(transform);
  const f32 inv_mass = 1.0f / JPH::max(desc.node_mass, 1.0e-6f);

  // Vertices carry the full transform baked in (body-local = world - origin,
  // body rotation stays identity); roots are kinematic.
  JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings;
  shared->mVertices.reserve(node_count);
  for (u32 s = 0; s < desc.strand_count; ++s) {
    for (u32 k = 0; k < pps; ++k) {
      const f32* lp = &desc.points[(static_cast<size_t>(s) * pps + k) * 3];
      Vec3 world = TransformPoint(transform, {lp[0], lp[1], lp[2]});
      JPH::SoftBodySharedSettings::Vertex v;
      v.mPosition = {world.x - origin.x, world.y - origin.y, world.z - origin.z};
      v.mInvMass = k == 0 ? 0.0f : inv_mass;
      shared->mVertices.push_back(v);
    }
  }
  for (u32 i = 0; i < desc.pin_count; ++i) {
    u32 s = desc.pins[i * 2], k = desc.pins[i * 2 + 1];
    if (s < desc.strand_count && k < pps) shared->mVertices[s * pps + k].mInvMass = 0.0f;
  }

  // One rod chain per strand, bend/twist between consecutive rods, and a long
  // range attachment from each node to the nearest pinned node up its strand
  // so fast head motion cannot stretch the hair.
  for (u32 s = 0; s < desc.strand_count; ++s) {
    const u32 vbase = s * pps;
    const u32 rbase = static_cast<u32>(shared->mRodStretchShearConstraints.size());
    u32 anchor = vbase;
    f32 arc = 0;
    for (u32 k = 1; k < pps; ++k) {
      shared->mRodStretchShearConstraints.push_back(JPH::SoftBodySharedSettings::RodStretchShear(
          vbase + k - 1, vbase + k, desc.stretch_compliance));
      if (k >= 2) {
        shared->mRodBendTwistConstraints.push_back(JPH::SoftBodySharedSettings::RodBendTwist(
            rbase + k - 2, rbase + k - 1, desc.bend_compliance));
      }
      if (shared->mVertices[vbase + k].mInvMass == 0.0f) {
        anchor = vbase + k;
        arc = 0;
        continue;
      }
      JPH::Vec3 a(shared->mVertices[vbase + k - 1].mPosition);
      JPH::Vec3 b(shared->mVertices[vbase + k].mPosition);
      arc += (b - a).Length();
      shared->mLRAConstraints.push_back(
          JPH::SoftBodySharedSettings::LRA(anchor, vbase + k, arc * desc.max_stretch));
    }
  }

  // Cross-strand ties (braid weave, dreadlock bundling) as edge springs at
  // their rest-pose distance.
  for (u32 i = 0; i < desc.bind_count; ++i) {
    const u32* b = &desc.binds[i * 4];
    if (b[0] >= desc.strand_count || b[1] >= pps || b[2] >= desc.strand_count || b[3] >= pps) {
      continue;
    }
    u32 v1 = b[0] * pps + b[1], v2 = b[2] * pps + b[3];
    if (v1 == v2) continue;
    if (shared->mVertices[v1].mInvMass == 0.0f && shared->mVertices[v2].mInvMass == 0.0f) continue;
    shared->mEdgeConstraints.push_back(
        JPH::SoftBodySharedSettings::Edge(v1, v2, desc.bind_compliance));
  }
  if (!shared->mEdgeConstraints.empty()) shared->CalculateEdgeLengths();
  shared->CalculateRodProperties();
  shared->Optimize();

  JPH::SoftBodyCreationSettings body(shared, ToJolt(origin), JPH::Quat::sIdentity(),
                                     layers::kDynamic);
  body.mNumIterations = JPH::max(desc.iterations, 1u);
  body.mLinearDamping = desc.damping;
  body.mGravityFactor = desc.gravity_factor;
  body.mVertexRadius = desc.node_radius;
  body.mUpdatePosition = false;  // the origin is the fixed groom anchor

  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  Impl::StrandGroomEntry entry;
  entry.body = bodies.CreateAndAddSoftBody(body, JPH::EActivation::Activate);
  if (entry.body.IsInvalid()) return 0;
  entry.alive = true;
  entry.node_count = node_count;

  for (u32 s = 0; s < desc.strand_count; ++s) {
    for (u32 k = 0; k < pps; ++k) {
      u32 v = s * pps + k;
      if (shared->mVertices[v].mInvMass != 0.0f) {
        entry.total_mass += desc.node_mass;
        continue;
      }
      const f32* lp = &desc.points[static_cast<size_t>(v) * 3];
      entry.pinned.push_back(v);
      entry.pinned_rest.push_back({lp[0], lp[1], lp[2]});
      entry.targets.push_back(JPH::Vec3(shared->mVertices[v].mPosition));
    }
  }

  // The character collision proxy: a kinematic compound of spheres/capsules
  // in the groom frame, moved with the groom transform like the NPC capsules.
  if (desc.sphere_count + desc.capsule_count > 0) {
    JPH::StaticCompoundShapeSettings compound;
    u32 added = 0;
    for (u32 i = 0; i < desc.sphere_count; ++i) {
      const StrandGroomDesc::Sphere& sph = desc.spheres[i];
      if (sph.radius < 1e-4f) continue;
      compound.AddShape(ToJolt(sph.center), JPH::Quat::sIdentity(),
                        new JPH::SphereShape(sph.radius));
      ++added;
    }
    for (u32 i = 0; i < desc.capsule_count; ++i) {
      const StrandGroomDesc::Capsule& cap = desc.capsules[i];
      JPH::Vec3 a = ToJolt(cap.a), b = ToJolt(cap.b);
      f32 half_height = (b - a).Length() * 0.5f;
      if (cap.radius < 1e-4f) continue;
      if (half_height < 1e-4f) {
        compound.AddShape(a, JPH::Quat::sIdentity(), new JPH::SphereShape(cap.radius));
      } else {
        compound.AddShape((a + b) * 0.5f,
                          JPH::Quat::sFromTo(JPH::Vec3::sAxisY(), (b - a).Normalized()),
                          new JPH::CapsuleShape(half_height, cap.radius));
      }
      ++added;
    }
    JPH::ShapeSettings::ShapeResult proxy_shape = compound.Create();
    if (added > 0 && proxy_shape.IsValid()) {
      JPH::BodyCreationSettings proxy(proxy_shape.Get(), ToJolt(origin),
                                      QuatFromColumns(transform.m), JPH::EMotionType::Kinematic,
                                      layers::kDynamic);
      entry.proxy = bodies.CreateAndAddBody(proxy, JPH::EActivation::Activate);
    }
  }

  impl_->strand_grooms.push_back(std::move(entry));
  return impl_->strand_grooms.size();
}

void PhysicsWorld::SetStrandGroomTransform(StrandGroomId id, const Mat4& transform, f32 dt) {
  if (!impl_ || id == 0 || id > impl_->strand_grooms.size()) return;
  Impl::StrandGroomEntry& entry = impl_->strand_grooms[id - 1];
  if (!entry.alive) return;
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();

  if (!entry.proxy.IsInvalid()) {
    JPH::RVec3 position = ToJolt(Translation(transform));
    if (dt > 0) {
      bodies.MoveKinematic(entry.proxy, position, QuatFromColumns(transform.m), dt);
    } else {
      bodies.SetPositionAndRotation(entry.proxy, position, QuatFromColumns(transform.m),
                                    JPH::EActivation::Activate);
    }
  }

  // Body-local pin targets; the origin never moves (mUpdatePosition off).
  const JPH::RVec3 origin = bodies.GetPosition(entry.body);
  for (size_t i = 0; i < entry.pinned.size(); ++i) {
    Vec3 world = TransformPoint(transform, entry.pinned_rest[i]);
    entry.targets[i] = JPH::Vec3(world.x - static_cast<f32>(origin.GetX()),
                                 world.y - static_cast<f32>(origin.GetY()),
                                 world.z - static_cast<f32>(origin.GetZ()));
  }
  if (dt <= 0) {
    JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), entry.body);
    if (lock.Succeeded()) {
      auto* soft =
          static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
      for (size_t i = 0; i < entry.pinned.size(); ++i) {
        JPH::SoftBodyVertex& v = soft->GetVertex(entry.pinned[i]);
        v.mPosition = entry.targets[i];
        v.mVelocity = JPH::Vec3::sZero();
      }
    }
  }
  bodies.ActivateBody(entry.body);
}

void PhysicsWorld::SetStrandGroomWind(StrandGroomId id, const Vec3& wind) {
  if (!impl_ || id == 0 || id > impl_->strand_grooms.size()) return;
  Impl::StrandGroomEntry& entry = impl_->strand_grooms[id - 1];
  if (!entry.alive) return;
  entry.wind = ToJolt(wind);
  if (!entry.wind.IsNearZero()) {
    impl_->system->GetBodyInterface().ActivateBody(entry.body);
  }
}

u32 PhysicsWorld::StrandGroomPositionCount(StrandGroomId id) const {
  if (!impl_ || id == 0 || id > impl_->strand_grooms.size()) return 0;
  const Impl::StrandGroomEntry& entry = impl_->strand_grooms[id - 1];
  return entry.alive ? entry.node_count * 3 : 0;
}

bool PhysicsWorld::GetStrandGroomPositions(StrandGroomId id, f32* out, u32 count) const {
  if (!impl_ || !out || id == 0 || id > impl_->strand_grooms.size()) return false;
  const Impl::StrandGroomEntry& entry = impl_->strand_grooms[id - 1];
  if (!entry.alive || count < entry.node_count * 3) return false;
  JPH::BodyLockRead lock(impl_->system->GetBodyLockInterface(), entry.body);
  if (!lock.Succeeded()) return false;
  const JPH::Body& body = lock.GetBody();
  const auto* soft =
      static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
  const JPH::RMat44 com = body.GetCenterOfMassTransform();
  const JPH::Array<JPH::SoftBodyVertex>& vertices = soft->GetVertices();
  for (size_t i = 0; i < vertices.size(); ++i) {
    JPH::RVec3 world = com * vertices[i].mPosition;
    out[i * 3 + 0] = static_cast<f32>(world.GetX());
    out[i * 3 + 1] = static_cast<f32>(world.GetY());
    out[i * 3 + 2] = static_cast<f32>(world.GetZ());
  }
  return true;
}

void PhysicsWorld::RemoveStrandGroom(StrandGroomId id) {
  if (!impl_ || id == 0 || id > impl_->strand_grooms.size()) return;
  Impl::StrandGroomEntry& entry = impl_->strand_grooms[id - 1];
  if (!entry.alive) return;
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  for (JPH::BodyID body : {entry.body, entry.proxy}) {
    if (body.IsInvalid()) continue;
    bodies.RemoveBody(body);
    bodies.DestroyBody(body);
  }
  entry.alive = false;
}

ClothId PhysicsWorld::CreateCloth(const ClothDesc& desc, const Mat4& transform) {
  if (!impl_ || !FeatureEnabled(FeatureId::kCloth) || !desc.positions || !desc.indices ||
      desc.vertex_count < 3 || desc.index_count < 3 || (desc.pin_count > 0 && !desc.pins) ||
      !IsFinite(transform)) {
    return 0;
  }
  if (!std::isfinite(desc.areal_density) || desc.areal_density <= 0 ||
      !std::isfinite(desc.warp_compliance) || desc.warp_compliance < 0 ||
      !std::isfinite(desc.weft_compliance) || desc.weft_compliance < 0 ||
      !std::isfinite(desc.shear_compliance) || desc.shear_compliance < 0 ||
      !std::isfinite(desc.bend_compliance) || desc.bend_compliance < 0 ||
      !std::isfinite(desc.max_stretch) || desc.max_stretch < 1 ||
      !std::isfinite(desc.collision_radius) || desc.collision_radius < 0 ||
      !std::isfinite(desc.self_collision_distance) || desc.self_collision_distance < 0 ||
      !std::isfinite(desc.self_collision_relaxation) || desc.self_collision_relaxation < 0 ||
      desc.self_collision_relaxation > 1 ||
      !std::isfinite(desc.aerodynamic_drag) || desc.aerodynamic_drag < 0 ||
      !std::isfinite(desc.damping) || desc.damping < 0 ||
      !std::isfinite(desc.gravity_factor) || !std::isfinite(desc.friction) || desc.friction < 0 ||
      !std::isfinite(desc.restitution) || desc.restitution < 0 ||
      !std::isfinite(desc.pressure) || desc.pressure < 0 ||
      !std::isfinite(desc.max_linear_velocity) || desc.max_linear_velocity <= 0 ||
      desc.iterations > 64 || desc.self_collision_iterations > 8 ||
      (desc.self_collision_distance > 0 && desc.self_collision_iterations == 0)) {
    RX_WARN("cloth rejected: invalid material or collision parameters");
    return 0;
  }

  // Bake the complete spawn transform into body-local vertices around a nearby
  // origin. Jolt keeps soft-body rotation identity for better solver accuracy.
  const Vec3 origin = Translation(transform);
  base::Vector<Vec3> positions;
  positions.reserve(desc.vertex_count);
  for (u32 i = 0; i < desc.vertex_count; ++i) {
    const Vec3 world = TransformPoint(transform, desc.positions[i]);
    positions.push_back(world - origin);
  }

  detail::ClothTopology topology;
  if (!detail::BuildClothTopology(positions.data(), desc.vertex_count, desc.indices,
                                  desc.index_count, &topology)) {
    RX_WARN("cloth rejected: topology is degenerate, duplicated, non-manifold or mis-wound");
    return 0;
  }
  const bool pressure_capable = topology.closed && topology.component_count == 1 &&
                                std::isfinite(topology.signed_volume) &&
                                topology.signed_volume > 1.0e-9f;
  if (desc.pressure > 0 && !pressure_capable) {
    RX_WARN("cloth rejected: pressure requires one closed, outward-wound volume");
    return 0;
  }
  if (desc.uvs) {
    for (u32 i = 0; i < desc.vertex_count * 2; ++i) {
      if (!std::isfinite(desc.uvs[i])) {
        RX_WARN("cloth rejected: non-finite material UV");
        return 0;
      }
    }
  }

  base::Vector<u8> pin_mask;
  pin_mask.resize(desc.vertex_count);
  std::fill(pin_mask.begin(), pin_mask.end(), 0);
  for (u32 i = 0; i < desc.pin_count; ++i) {
    const u32 vertex = desc.pins[i];
    if (vertex >= desc.vertex_count || pin_mask[vertex]) {
      RX_WARN("cloth rejected: pin index is invalid or duplicated");
      return 0;
    }
    pin_mask[vertex] = 1;
  }

  if ((desc.joint_count > 0 || desc.skin_constraint_count > 0) &&
      (!desc.inverse_bind_matrices || !desc.skin_constraints || desc.joint_count == 0 ||
       desc.skin_constraint_count == 0)) {
    RX_WARN("cloth rejected: skin constraints require inverse bind matrices");
    return 0;
  }
  base::Vector<u8> skinned;
  base::Vector<u8> hard_skinned;
  base::Vector<u8> backstopped;
  skinned.resize(desc.vertex_count);
  hard_skinned.resize(desc.vertex_count);
  backstopped.resize(desc.vertex_count);
  std::fill(skinned.begin(), skinned.end(), 0);
  std::fill(hard_skinned.begin(), hard_skinned.end(), 0);
  std::fill(backstopped.begin(), backstopped.end(), 0);
  for (u32 i = 0; i < desc.skin_constraint_count; ++i) {
    const ClothSkinConstraint& skin = desc.skin_constraints[i];
    if (skin.vertex >= desc.vertex_count || skinned[skin.vertex] || pin_mask[skin.vertex] ||
        !std::isfinite(skin.max_distance) || skin.max_distance < 0 ||
        !std::isfinite(skin.backstop_distance) || !std::isfinite(skin.backstop_radius) ||
        skin.backstop_radius < 0) {
      RX_WARN("cloth rejected: invalid skin constraint");
      return 0;
    }
    u32 weight_count = 0;
    for (const ClothSkinWeight& weight : skin.weights) {
      if (weight.weight == 0) continue;
      if (!std::isfinite(weight.weight) || weight.weight < 0 || weight.joint >= desc.joint_count ||
          weight_count >= JPH::SoftBodySharedSettings::Skinned::cMaxSkinWeights) {
        RX_WARN("cloth rejected: invalid skin weight");
        return 0;
      }
      ++weight_count;
    }
    if (weight_count == 0) {
      RX_WARN("cloth rejected: skin constraint has no positive weights");
      return 0;
    }
    skinned[skin.vertex] = 1;
    hard_skinned[skin.vertex] = skin.max_distance == 0;
    backstopped[skin.vertex] = skin.backstop_distance < skin.max_distance;
  }
  base::Vector<u32> skinned_face_counts;
  skinned_face_counts.resize(desc.vertex_count);
  std::fill(skinned_face_counts.begin(), skinned_face_counts.end(), 0);
  for (u32 face = 0; face < desc.index_count; face += 3) {
    const u32 a = desc.indices[face + 0];
    const u32 b = desc.indices[face + 1];
    const u32 c = desc.indices[face + 2];
    if (skinned[a] && skinned[b] && skinned[c]) {
      for (u32 vertex : {a, b, c}) {
        if (++skinned_face_counts[vertex] >= 256) {
          RX_WARN("cloth rejected: a skinned vertex has too many incident faces");
          return 0;
        }
      }
    } else if (backstopped[a] || backstopped[b] || backstopped[c]) {
      RX_WARN("cloth rejected: backstops require fully skinned incident faces");
      return 0;
    }
  }

  base::Vector<f32> inverse_masses;
  inverse_masses.resize(desc.vertex_count);
  if (desc.inverse_masses) {
    for (u32 i = 0; i < desc.vertex_count; ++i) {
      if (!std::isfinite(desc.inverse_masses[i]) || desc.inverse_masses[i] < 0) {
        RX_WARN("cloth rejected: inverse masses must be finite and non-negative");
        return 0;
      }
      inverse_masses[i] = desc.inverse_masses[i];
    }
  } else {
    base::Vector<f64> masses;
    masses.resize(desc.vertex_count);
    std::fill(masses.begin(), masses.end(), 0.0);
    for (size_t i = 0; i < topology.indices.size(); i += 3) {
      const u32 a = topology.indices[i + 0];
      const u32 b = topology.indices[i + 1];
      const u32 c = topology.indices[i + 2];
      const f64 triangle_mass =
          static_cast<f64>(topology.triangle_areas[i / 3]) * desc.areal_density;
      if (!std::isfinite(triangle_mass)) return 0;
      masses[a] += triangle_mass / 3.0;
      masses[b] += triangle_mass / 3.0;
      masses[c] += triangle_mass / 3.0;
    }
    for (u32 i = 0; i < desc.vertex_count; ++i) {
      const f64 inverse_mass = 1.0 / masses[i];
      if (masses[i] <= 1.0e-8 || !std::isfinite(masses[i]) ||
          !std::isfinite(inverse_mass) || inverse_mass > std::numeric_limits<f32>::max()) {
        RX_WARN("cloth rejected: every vertex must belong to a non-degenerate face");
        return 0;
      }
      inverse_masses[i] = static_cast<f32>(inverse_mass);
    }
  }
  for (u32 i = 0; i < desc.pin_count; ++i) inverse_masses[desc.pins[i]] = 0;
  for (u32 i = 0; i < desc.vertex_count; ++i) {
    if (hard_skinned[i]) inverse_masses[i] = 0;
  }

  JPH::Ref<JPH::SoftBodySharedSettings> shared = new JPH::SoftBodySharedSettings;
  shared->mVertices.reserve(desc.vertex_count);
  for (u32 i = 0; i < desc.vertex_count; ++i) {
    JPH::SoftBodySharedSettings::Vertex vertex;
    vertex.mPosition = {positions[i].x, positions[i].y, positions[i].z};
    vertex.mInvMass = inverse_masses[i];
    shared->mVertices.push_back(vertex);
  }
  shared->mFaces.reserve(desc.index_count / 3);
  for (u32 i = 0; i < desc.index_count; i += 3) {
    shared->mFaces.emplace_back(desc.indices[i + 0], desc.indices[i + 1], desc.indices[i + 2]);
  }

  JPH::SoftBodySharedSettings::VertexAttributes attributes;
  attributes.mCompliance = 0.5f * (desc.warp_compliance + desc.weft_compliance);
  attributes.mShearCompliance = desc.shear_compliance;
  attributes.mBendCompliance = desc.bend_compliance;
  attributes.mLRAMaxDistanceMultiplier = desc.max_stretch;
  switch (desc.lra_mode) {
    case ClothLraMode::kNone:
      attributes.mLRAType = JPH::SoftBodySharedSettings::ELRAType::None;
      break;
    case ClothLraMode::kEuclidean:
      attributes.mLRAType = JPH::SoftBodySharedSettings::ELRAType::EuclideanDistance;
      break;
    case ClothLraMode::kGeodesic:
      attributes.mLRAType = JPH::SoftBodySharedSettings::ELRAType::GeodesicDistance;
      break;
  }
  JPH::SoftBodySharedSettings::EBendType bend = JPH::SoftBodySharedSettings::EBendType::Dihedral;
  switch (desc.bend_model) {
    case ClothBendModel::kNone:
      bend = JPH::SoftBodySharedSettings::EBendType::None;
      break;
    case ClothBendModel::kDistance:
      bend = JPH::SoftBodySharedSettings::EBendType::Distance;
      break;
    case ClothBendModel::kDihedral:
      bend = JPH::SoftBodySharedSettings::EBendType::Dihedral;
      break;
  }
  shared->CreateConstraints(&attributes, 1, bend);

  // Jolt identifies shear diagonals while generating constraints. Topology
  // edges and generated diagonal shears get fabric-oriented compliance; the
  // extra non-diagonal springs in distance-bend mode retain bend compliance.
  if (desc.uvs) {
    auto is_topology_edge = [&](u32 a, u32 b) {
      if (a > b) std::swap(a, b);
      size_t low = 0, high = topology.edges.size() / 2;
      while (low < high) {
        const size_t middle = (low + high) / 2;
        const u32 edge_a = topology.edges[middle * 2 + 0];
        const u32 edge_b = topology.edges[middle * 2 + 1];
        if (edge_a < a || (edge_a == a && edge_b < b))
          low = middle + 1;
        else
          high = middle;
      }
      return low < topology.edges.size() / 2 && topology.edges[low * 2 + 0] == a &&
             topology.edges[low * 2 + 1] == b;
    };
    for (JPH::SoftBodySharedSettings::Edge& edge : shared->mEdgeConstraints) {
      const f32 du = std::abs(desc.uvs[edge.mVertex[1] * 2 + 0] -
                              desc.uvs[edge.mVertex[0] * 2 + 0]);
      const f32 dv = std::abs(desc.uvs[edge.mVertex[1] * 2 + 1] -
                              desc.uvs[edge.mVertex[0] * 2 + 1]);
      const bool warp = du > 2.0f * dv;
      const bool weft = dv > 2.0f * du;
      if (desc.bend_model == ClothBendModel::kDistance &&
          !is_topology_edge(edge.mVertex[0], edge.mVertex[1])) {
        continue;
      }
      if (warp) {
        edge.mCompliance = desc.warp_compliance;
      } else if (weft) {
        edge.mCompliance = desc.weft_compliance;
      } else {
        edge.mCompliance = desc.shear_compliance;
      }
    }
  }

  Mat4 spawn_linear = transform;
  spawn_linear.m[12] = spawn_linear.m[13] = spawn_linear.m[14] = 0;
  spawn_linear.m[3] = spawn_linear.m[7] = spawn_linear.m[11] = 0;
  spawn_linear.m[15] = 1;
  const Mat4 inverse_spawn_linear = Inverse(spawn_linear);
  for (u32 joint = 0; joint < desc.joint_count; ++joint) {
    if (!IsFinite(desc.inverse_bind_matrices[joint])) {
      RX_WARN("cloth rejected: non-finite inverse bind matrix");
      return 0;
    }
    shared->mInvBindMatrices.emplace_back(
        joint, ToJolt(desc.inverse_bind_matrices[joint] * inverse_spawn_linear));
  }
  for (u32 i = 0; i < desc.skin_constraint_count; ++i) {
    const ClothSkinConstraint& skin = desc.skin_constraints[i];
    JPH::SoftBodySharedSettings::Skinned native(
        skin.vertex, skin.max_distance, skin.backstop_distance, skin.backstop_radius);
    u32 weight_count = 0;
    for (const ClothSkinWeight& weight : skin.weights) {
      if (weight.weight == 0) continue;
      native.mWeights[weight_count++] =
          JPH::SoftBodySharedSettings::SkinWeight(weight.joint, weight.weight);
    }
    native.NormalizeWeights();
    shared->mSkinnedConstraints.push_back(native);
  }
  if (!shared->mSkinnedConstraints.empty()) shared->CalculateSkinnedConstraintNormals();
  shared->Optimize();

  JPH::SoftBodyCreationSettings body(shared, ToJolt(origin), JPH::Quat::sIdentity(),
                                      layers::kDynamic);
  body.mNumIterations = std::max(desc.iterations, 1u);
  body.mLinearDamping = std::max(desc.damping, 0.0f);
  body.mGravityFactor = desc.gravity_factor;
  body.mFriction = std::max(desc.friction, 0.0f);
  body.mRestitution = std::max(desc.restitution, 0.0f);
  body.mPressure = desc.pressure;
  body.mVertexRadius = desc.collision_radius;
  body.mMaxLinearVelocity = std::max(desc.max_linear_velocity, 0.01f);
  body.mUpdatePosition = desc.update_position;
  body.mMakeRotationIdentity = true;
  body.mAllowSleeping = desc.allow_sleeping;
  body.mFacesDoubleSided = desc.faces_double_sided;

  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  Impl::ClothEntry entry;
  entry.body = bodies.CreateAndAddSoftBody(body, JPH::EActivation::Activate);
  if (entry.body.IsInvalid()) return 0;
  entry.alive = true;
  entry.pressure_capable = pressure_capable;
  entry.vertex_count = desc.vertex_count;
  entry.joint_count = desc.joint_count;
  entry.skin_constraint_count = desc.skin_constraint_count;
  entry.aerodynamic_drag = desc.aerodynamic_drag;
  entry.gravity_factor = desc.gravity_factor;
  entry.max_linear_velocity = desc.max_linear_velocity;
  entry.collision_inverse_masses = std::move(inverse_masses);
  entry.topology = std::move(topology);
  entry.self_collision.distance = desc.self_collision_distance;
  entry.self_collision.relaxation = desc.self_collision_relaxation;
  entry.self_collision.max_velocity = desc.max_linear_velocity;
  entry.self_collision.iterations = desc.self_collision_iterations;
  for (u32 i = 0; i < desc.pin_count; ++i) {
    const u32 vertex = desc.pins[i];
    entry.pinned.push_back(vertex);
    entry.pinned_rest.push_back(desc.positions[vertex]);
    entry.targets.push_back(TransformPoint(transform, desc.positions[vertex]));
  }
  if (entry.skin_constraint_count > 0) {
    JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), entry.body);
    if (!lock.Succeeded()) {
      bodies.RemoveBody(entry.body);
      bodies.DestroyBody(entry.body);
      return 0;
    }
    auto* soft =
        static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    soft->SetEnableSkinConstraints(false);
  }
  impl_->cloth.push_back(std::move(entry));
  return impl_->cloth.size();
}

bool PhysicsWorld::SetClothTransform(ClothId id, const Mat4& transform, f32 dt) {
  if (!impl_ || id == 0 || id > impl_->cloth.size() || !IsFinite(transform) ||
      !std::isfinite(dt) || dt < 0) {
    return false;
  }
  Impl::ClothEntry& entry = impl_->cloth[id - 1];
  if (!entry.alive || entry.pinned.empty()) return false;
  entry.target_scratch.resize(entry.pinned_rest.size());
  for (size_t i = 0; i < entry.pinned_rest.size(); ++i) {
    entry.target_scratch[i] = TransformPoint(transform, entry.pinned_rest[i]);
  }
  return SetClothPinTargets(id, entry.target_scratch.data(),
                            static_cast<u32>(entry.target_scratch.size()), dt);
}

bool PhysicsWorld::SetClothPinTargets(ClothId id, const Vec3* targets, u32 target_count, f32 dt) {
  if (!impl_ || id == 0 || id > impl_->cloth.size() || !std::isfinite(dt) || dt < 0) return false;
  Impl::ClothEntry& entry = impl_->cloth[id - 1];
  if (!entry.alive || target_count != entry.pinned.size() || (target_count > 0 && !targets)) {
    return false;
  }
  bool changed = false;
  for (u32 i = 0; i < target_count; ++i) {
    if (!IsFinite(targets[i])) return false;
    changed = changed || Length(targets[i] - entry.targets[i]) > 1.0e-7f;
  }
  if (!changed) return true;

  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  entry.targets.assign(targets, targets + target_count);
  entry.pin_time_remaining = dt;
  if (dt == 0 && target_count > 0) {
    JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), entry.body);
    if (!lock.Succeeded()) return false;
    const JPH::Mat44 to_local =
        lock.GetBody().GetCenterOfMassTransform().InversedRotationTranslation().ToMat44();
    auto* soft =
        static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    for (u32 i = 0; i < target_count; ++i) {
      JPH::SoftBodyVertex& vertex = soft->GetVertex(entry.pinned[i]);
      const JPH::Vec3 local_target = to_local * ToJolt(entry.targets[i]);
      vertex.mPreviousPosition = local_target;
      vertex.mPosition = local_target;
      vertex.mVelocity = JPH::Vec3::sZero();
    }
  }
  bodies.ActivateBody(entry.body);
  return true;
}

bool PhysicsWorld::SetClothJointTransforms(ClothId id, const Mat4* world_joints, u32 joint_count,
                                           bool hard_reset) {
  if (!impl_ || id == 0 || id > impl_->cloth.size()) return false;
  Impl::ClothEntry& entry = impl_->cloth[id - 1];
  if (!entry.alive || entry.skin_constraint_count == 0 || !world_joints ||
      joint_count < entry.joint_count) {
    return false;
  }
  bool changed = hard_reset || entry.last_joint_transforms.size() != entry.joint_count;
  for (u32 i = 0; i < entry.joint_count; ++i) {
    if (!IsFinite(world_joints[i])) return false;
    if (!changed) {
      for (u32 element = 0; element < 16; ++element) {
        if (std::abs(world_joints[i].m[element] - entry.last_joint_transforms[i].m[element]) >
            1.0e-7f) {
          changed = true;
          break;
        }
      }
    }
  }
  if (!changed) return true;
  {
    JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), entry.body);
    if (!lock.Succeeded()) return false;
    JPH::Body& body = lock.GetBody();
    const JPH::RMat44 center_of_mass = body.GetCenterOfMassTransform();
    const JPH::Mat44 to_local = center_of_mass.InversedRotationTranslation().ToMat44();
    entry.skin_pose.clear();
    entry.skin_pose.reserve(entry.joint_count);
    for (u32 i = 0; i < entry.joint_count; ++i) {
      entry.skin_pose.push_back(to_local * ToJolt(world_joints[i]));
    }
    auto* soft = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
    const bool initialize_skin = entry.last_joint_transforms.empty();
    soft->SkinVertices(center_of_mass, entry.skin_pose.data(), entry.joint_count,
                        hard_reset || initialize_skin, impl_->skin_allocator);
    soft->SetEnableSkinConstraints(true);
  }
  entry.last_joint_transforms.assign(world_joints, world_joints + entry.joint_count);
  impl_->system->GetBodyInterface().ActivateBody(entry.body);
  return true;
}

void PhysicsWorld::SetClothWind(ClothId id, const Vec3& velocity) {
  if (!impl_ || id == 0 || id > impl_->cloth.size() || !IsFinite(velocity)) return;
  Impl::ClothEntry& entry = impl_->cloth[id - 1];
  if (!entry.alive) return;
  const JPH::Vec3 previous = entry.wind;
  entry.wind = ToJolt(velocity);
  if (!entry.wind.IsClose(previous)) impl_->system->GetBodyInterface().ActivateBody(entry.body);
}

void PhysicsWorld::SetClothPressure(ClothId id, f32 pressure) {
  if (!impl_ || id == 0 || id > impl_->cloth.size() || !std::isfinite(pressure) || pressure < 0) {
    return;
  }
  Impl::ClothEntry& entry = impl_->cloth[id - 1];
  if (!entry.alive || !entry.pressure_capable) return;
  bool changed = false;
  {
    JPH::BodyLockWrite lock(impl_->system->GetBodyLockInterface(), entry.body);
    if (!lock.Succeeded()) return;
    auto* soft =
        static_cast<JPH::SoftBodyMotionProperties*>(lock.GetBody().GetMotionProperties());
    changed = soft->GetPressure() != pressure;
    if (changed) soft->SetPressure(pressure);
  }
  if (changed) impl_->system->GetBodyInterface().ActivateBody(entry.body);
}

u32 PhysicsWorld::ClothVertexCount(ClothId id) const {
  if (!impl_ || id == 0 || id > impl_->cloth.size()) return 0;
  const Impl::ClothEntry& entry = impl_->cloth[id - 1];
  return entry.alive ? entry.vertex_count : 0;
}

bool PhysicsWorld::GetClothPositions(ClothId id, Vec3* out, u32 count) const {
  if (!impl_ || !out || id == 0 || id > impl_->cloth.size()) return false;
  const Impl::ClothEntry& entry = impl_->cloth[id - 1];
  if (!entry.alive || count < entry.vertex_count) return false;
  JPH::BodyLockRead lock(impl_->system->GetBodyLockInterface(), entry.body);
  if (!lock.Succeeded()) return false;
  const JPH::Body& body = lock.GetBody();
  const auto* soft =
      static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
  const JPH::RMat44 center_of_mass = body.GetCenterOfMassTransform();
  const JPH::Array<JPH::SoftBodyVertex>& vertices = soft->GetVertices();
  for (u32 i = 0; i < entry.vertex_count; ++i) {
    const JPH::RVec3 world = center_of_mass * vertices[i].mPosition;
    out[i] = {static_cast<f32>(world.GetX()), static_cast<f32>(world.GetY()),
              static_cast<f32>(world.GetZ())};
  }
  return true;
}

void PhysicsWorld::RemoveCloth(ClothId id) {
  if (!impl_ || id == 0 || id > impl_->cloth.size()) return;
  Impl::ClothEntry& entry = impl_->cloth[id - 1];
  if (!entry.alive) return;
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  bodies.RemoveBody(entry.body);
  bodies.DestroyBody(entry.body);
  entry = Impl::ClothEntry{};
}

bool PhysicsWorld::Raycast(const Vec3& origin, const Vec3& direction, f32 max_distance,
                           RayHit* out) const {
  return Raycast(origin, direction, max_distance, out, 0);
}

bool PhysicsWorld::Raycast(const Vec3& origin, const Vec3& direction, f32 max_distance, RayHit* out,
                           BodyId ignore) const {
  if (!impl_) return false;
  Vec3 dir = Normalize(direction);
  JPH::RRayCast ray{ToJolt(origin), ToJolt(dir) * max_distance};
  JPH::RayCastResult result;
  // ignore == 0 maps to the invalid body id, so the filter skips nothing.
  JPH::IgnoreSingleBodyFilter body_filter(JPH::BodyID(static_cast<JPH::uint32>(ignore - 1)));
  if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, body_filter)) return false;
  JPH::RVec3 hit = ray.GetPointOnRay(result.mFraction);
  out->position = {static_cast<f32>(hit.GetX()), static_cast<f32>(hit.GetY()),
                   static_cast<f32>(hit.GetZ())};
  out->distance = result.mFraction * max_distance;
  out->normal = {0, 1, 0};
  JPH::BodyLockRead lock(impl_->system->GetBodyLockInterface(), result.mBodyID);
  if (lock.Succeeded()) {
    JPH::Vec3 n = lock.GetBody().GetWorldSpaceSurfaceNormal(result.mSubShapeID2, hit);
    out->normal = {n.GetX(), n.GetY(), n.GetZ()};
  }
  return true;
}

bool PhysicsWorld::GetBodyTransform(BodyId id, Vec3* position, f32 rotation[4]) const {
  if (!impl_ || id == 0) return false;
  JPH::BodyID body(static_cast<JPH::uint32>(id - 1));
  JPH::BodyInterface& bodies = impl_->system->GetBodyInterface();
  JPH::RVec3 p;
  JPH::Quat q;
  bodies.GetPositionAndRotation(body, p, q);
  *position = {static_cast<f32>(p.GetX()), static_cast<f32>(p.GetY()),
               static_cast<f32>(p.GetZ())};
  rotation[0] = q.GetX();
  rotation[1] = q.GetY();
  rotation[2] = q.GetZ();
  rotation[3] = q.GetW();
  return true;
}

}  // namespace rx::physics
