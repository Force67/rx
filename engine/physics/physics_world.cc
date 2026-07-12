#include "physics/physics_world.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
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

#include "core/log.h"

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

// Friction of static world geometry (ground, roads, walls). Jolt's default
// of 0.2 is ice: vehicle tires combine as sqrt(tire * body), so the body
// side must carry a real asphalt/concrete-grade value.
constexpr f32 kStaticFriction = 0.9f;

}  // namespace

struct PhysicsWorld::Impl {
  BroadPhaseLayers broad_phase_layers;
  ObjectVsBroadPhase object_vs_broad_phase;
  ObjectLayerPair object_layer_pair;
  std::unique_ptr<JPH::TempAllocatorImpl> temp_allocator;
  std::unique_ptr<JPH::JobSystemThreadPool> job_system;
  std::unique_ptr<JPH::PhysicsSystem> system;
  base::Vector<JPH::BodyID> dynamic_bodies;
  base::UnorderedMap<u64, JPH::RefConst<JPH::Shape>> mesh_shapes;
  base::Vector<JPH::Ref<JPH::GroupFilterTable>> filter_groups;
  struct CharacterEntry {
    JPH::Ref<JPH::CharacterVirtual> character;
    f32 vy = 0;  // tracked vertical velocity (gravity + jump)
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
    bool traction_control = false;
    f32 tc_scale = 1;  // smoothed traction-control throttle authority
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
  impl_->temp_allocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
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
      bodies.AddForce(entry.body, up * (-entry.downforce * v * v));
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

  impl_->system->Update(dt, 1, impl_->temp_allocator.get(), impl_->job_system.get());
}

BodyId PhysicsWorld::AddStaticBox(const Vec3& position, const Vec3& half_extent) {
  if (!impl_) return 0;
  JPH::BodyCreationSettings settings(new JPH::BoxShape(ToJolt(half_extent)), ToJolt(position),
                                     JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                     layers::kStatic);
  settings.mFriction = kStaticFriction;
  JPH::BodyID id = impl_->system->GetBodyInterface().CreateAndAddBody(
      settings, JPH::EActivation::DontActivate);
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddStaticMesh(const asset::Mesh& mesh, const Vec3& position,
                                   const f32 rotation[4], f32 scale) {
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
  JPH::Ref<JPH::ShapeSettings> shape_settings =
      new JPH::MeshShapeSettings(std::move(vertices), std::move(triangles));
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
                                           f32 scale) {
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
  return id.GetIndexAndSequenceNumber() + 1;
}

BodyId PhysicsWorld::AddHeightField(const Vec3& origin, const f32* heights, u32 samples,
                                    f32 size) {
  if (!impl_ || samples < 2) return 0;
  JPH::HeightFieldShapeSettings shape(heights, ToJolt(origin),
                                      {size / static_cast<f32>(samples - 1), 1.0f,
                                       size / static_cast<f32>(samples - 1)},
                                      samples);
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
JPH::Ref<JPH::Shape> BuildShape(const rx::physics::ShapeDesc& desc, f32 scale) {
  using Kind = rx::physics::ShapeDesc::Kind;
  switch (desc.kind) {
    case Kind::kSphere: {
      if (desc.radius * scale < 1e-4f) return nullptr;
      return new JPH::SphereShape(desc.radius * scale);
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
      if (half_height < 1e-4f) return new JPH::SphereShape(radius);
      JPH::Ref<JPH::Shape> capsule = new JPH::CapsuleShape(half_height, radius);
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
      return new JPH::BoxShape(half, JPH::min(0.05f, min_extent * 0.5f));
    }
    case Kind::kConvexHull: {
      if (desc.vertices.size() < 4) return nullptr;
      JPH::Array<JPH::Vec3> points;
      points.reserve(desc.vertices.size());
      for (const auto& v : desc.vertices) points.emplace_back(v.x * scale, v.y * scale,
                                                              v.z * scale);
      JPH::ConvexHullShapeSettings hull(points);
      auto result = hull.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kCompound: {
      JPH::StaticCompoundShapeSettings compound;
      u32 added = 0;
      for (const auto& child : desc.children) {
        if (child.kind == Kind::kPlaced && !child.children.empty()) {
          JPH::Ref<JPH::Shape> inner = BuildShape(child.children[0], scale);
          if (!inner) continue;
          JPH::Vec3 origin(child.transform[12], child.transform[13], child.transform[14]);
          compound.AddShape(origin * scale, QuatFromColumns(child.transform), inner);
          ++added;
        } else if (JPH::Ref<JPH::Shape> inner = BuildShape(child, scale)) {
          compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), inner);
          ++added;
        }
      }
      if (added == 0) return nullptr;
      if (added == 1 && desc.children.size() == 1 &&
          desc.children[0].kind != Kind::kPlaced) {
        return BuildShape(desc.children[0], scale);  // trivial list
      }
      auto result = compound.Create();
      return result.IsValid() ? result.Get() : JPH::Ref<JPH::Shape>();
    }
    case Kind::kPlaced: {
      if (desc.children.empty()) return nullptr;
      JPH::Ref<JPH::Shape> inner = BuildShape(desc.children[0], scale);
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
                                    const f32 rotation[4], f32 scale) {
  if (!impl_) return 0;
  JPH::Ref<JPH::Shape> shape = BuildShape(desc, scale);
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
  JPH::CharacterVirtual* character = impl_->characters[id - 1].character;

  character->SetLinearVelocity(ToJolt(velocity));
  JPH::CharacterVirtual::ExtendedUpdateSettings update;
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
      JPH::OffsetCenterOfMassShapeSettings(JPH::Vec3(0, -desc.com_drop, 0),
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
    if (desc.max_brake_torque > 0) w->mMaxBrakeTorque = desc.max_brake_torque;
    if (desc.suspension_frequency > 0) w->mSuspensionSpring.mFrequency = desc.suspension_frequency;
    if (desc.suspension_damping > 0) w->mSuspensionSpring.mDamping = desc.suspension_damping;
    // Tire grip: scale Jolt's default slip curves.
    if (desc.tire_long_friction > 0) {
      for (JPH::LinearCurve::Point& p : w->mLongitudinalFriction.mPoints)
        p.mY *= desc.tire_long_friction;
    }
    if (desc.tire_lat_friction > 0) {
      for (JPH::LinearCurve::Point& p : w->mLateralFriction.mPoints) p.mY *= desc.tire_lat_friction;
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
  // awd_front_split.
  const f32 final_drive = desc.final_drive > 0 ? desc.final_drive : 3.42f;
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
  vehicle.mController = controller;

  // Anti-roll bars per axle, like the Jolt vehicle sample.
  vehicle.mAntiRollBars.resize(2);
  vehicle.mAntiRollBars[0].mLeftWheel = 0;
  vehicle.mAntiRollBars[0].mRightWheel = 1;
  vehicle.mAntiRollBars[1].mLeftWheel = 2;
  vehicle.mAntiRollBars[1].mRightWheel = 3;
  if (desc.anti_roll_stiffness > 0) {
    vehicle.mAntiRollBars[0].mStiffness = desc.anti_roll_stiffness;
    vehicle.mAntiRollBars[1].mStiffness = desc.anti_roll_stiffness;
  }

  JPH::Ref<JPH::VehicleConstraint> constraint = new JPH::VehicleConstraint(*body, vehicle);
  constraint->SetVehicleCollisionTester(
      new JPH::VehicleCollisionTesterCastCylinder(layers::kDynamic, 0.05f));
  impl_->system->AddConstraint(constraint);
  impl_->system->AddStepListener(constraint);

  impl_->vehicles.push_back(
      {body->GetID(), constraint, true, 4, desc.downforce, desc.traction_control});
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

  impl_->vehicles.push_back(
      {body->GetID(), constraint, true, 2, desc.downforce, desc.traction_control});
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

void PhysicsWorld::DriveVehicle(VehicleId id, f32 forward, f32 right, f32 brake, f32 handbrake) {
  if (!impl_ || id == 0 || id > impl_->vehicles.size()) return;
  Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return;
  auto* controller = static_cast<JPH::WheeledVehicleController*>(entry.constraint->GetController());
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

bool PhysicsWorld::GetVehicleState(VehicleId id, VehicleState* out) const {
  if (!impl_ || id == 0 || id > impl_->vehicles.size() || !out) return false;
  const Impl::VehicleEntry& entry = impl_->vehicles[id - 1];
  if (!entry.alive) return false;

  const auto* controller =
      static_cast<const JPH::WheeledVehicleController*>(entry.constraint->GetController());
  out->rpm = controller->GetEngine().GetCurrentRPM();
  out->gear = controller->GetTransmission().GetCurrentGear();
  out->forward_speed = VehicleForwardSpeed(id);
  out->wheel_count = entry.wheel_count;
  for (u32 i = 0; i < entry.wheel_count && i < 4; ++i) {
    const auto* wheel = static_cast<const JPH::WheelWV*>(entry.constraint->GetWheel(i));
    VehicleState::WheelState& ws = out->wheels[i];
    ws.contact = wheel->HasContact();
    ws.suspension_length = wheel->GetSuspensionLength();
    ws.longitudinal_slip = wheel->mLongitudinalSlip;
    ws.angular_velocity = wheel->GetAngularVelocity();
    ws.rotation_angle = wheel->GetRotationAngle();
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

bool PhysicsWorld::Raycast(const Vec3& origin, const Vec3& direction, f32 max_distance,
                           RayHit* out) const {
  if (!impl_) return false;
  Vec3 dir = Normalize(direction);
  JPH::RRayCast ray{ToJolt(origin), ToJolt(dir) * max_distance};
  JPH::RayCastResult result;
  if (!impl_->system->GetNarrowPhaseQuery().CastRay(ray, result)) return false;
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
