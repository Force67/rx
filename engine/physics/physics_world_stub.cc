// Built when Jolt is unavailable; every call is a no-op.
#include "physics/physics_world.h"

namespace rx::physics {

struct PhysicsWorld::Impl {};

PhysicsWorld::PhysicsWorld() = default;
PhysicsWorld::~PhysicsWorld() = default;
bool PhysicsWorld::Initialize() { return false; }
void PhysicsWorld::Update(f32) {}
BodyId PhysicsWorld::AddStaticBox(const Vec3&, const Vec3&) { return 0; }
BodyId PhysicsWorld::AddStaticMesh(const asset::Mesh&, const Vec3&, const f32[4], f32) {
  return 0;
}
BodyId PhysicsWorld::AddHeightField(const Vec3&, const f32*, u32, f32) { return 0; }
bool PhysicsWorld::RegisterMeshShape(u64, const asset::Mesh&) { return false; }
bool PhysicsWorld::has_mesh_shape(u64) const { return false; }
BodyId PhysicsWorld::AddStaticMeshInstance(u64, const Vec3&, const f32[4], f32) { return 0; }
BodyId PhysicsWorld::AddDynamicBox(const Vec3&, const Vec3&, f32, const Vec3&) { return 0; }
BodyId PhysicsWorld::AddStaticShape(const ShapeDesc&, const Vec3&, const f32[4], f32) {
  return 0;
}
BodyId PhysicsWorld::AddDynamicShape(const ShapeDesc&, const Vec3&, const f32[4], f32, f32, f32,
                                     f32, i32, u32) {
  return 0;
}
i32 PhysicsWorld::CreateBodyFilterGroup(u32) { return -1; }
void PhysicsWorld::DisableFilterPair(i32, u32, u32) {}
JointId PhysicsWorld::AddSwingTwistJoint(BodyId, BodyId, const f32[12], const f32[12], f32, f32,
                                         f32, f32, f32, f32) {
  return 0;
}
JointId PhysicsWorld::AddHingeJoint(BodyId, BodyId, const f32[12], const f32[12], f32, f32, f32) {
  return 0;
}
void PhysicsWorld::EnableJointMotors(JointId, f32, f32) {}
void PhysicsWorld::SetJointMotorTarget(JointId, const f32[4]) {}
bool PhysicsWorld::GetJointOrientation(JointId, f32[4]) const { return false; }
void PhysicsWorld::SetJointMotorTorqueLimit(JointId, f32) {}
void PhysicsWorld::DisableJointMotors(JointId) {}
void PhysicsWorld::ApplyImpulse(BodyId, const Vec3&) {}
void PhysicsWorld::SetBodyKinematic(BodyId) {}
bool PhysicsWorld::GetBodyVelocity(BodyId, Vec3*, Vec3*) const { return false; }
f32 PhysicsWorld::GetBodyMass(BodyId) const { return 0; }
bool PhysicsWorld::GetBodyCenterOfMass(BodyId, Vec3*) const { return false; }
void PhysicsWorld::ApplyForce(BodyId, const Vec3&) {}
void PhysicsWorld::ApplyTorque(BodyId, const Vec3&) {}
Vec3 PhysicsWorld::gravity() const { return {0, -9.81f, 0}; }
void PhysicsWorld::WatchBodyContacts(BodyId) {}
void PhysicsWorld::UnwatchBodyContacts(BodyId) {}
u32 PhysicsWorld::GetBodyContacts(BodyId, BodyContact*, u32) const { return 0; }
BodyId PhysicsWorld::AddDynamicSphere(const Vec3&, f32, f32, const Vec3&) { return 0; }
BodyId PhysicsWorld::AddKinematicCapsule(const Vec3&, f32, f32) { return 0; }
BodyId PhysicsWorld::AddKinematicBox(const Vec3&, const Vec3&) { return 0; }
void PhysicsWorld::SetBodyPosition(BodyId, const Vec3&, const f32[4]) {}
void PhysicsWorld::MoveBodyKinematic(BodyId, const Vec3&, const f32[4], f32) {}
void PhysicsWorld::RemoveBody(BodyId) {}
VehicleId PhysicsWorld::CreateVehicle(const VehicleDesc&, const Vec3&, f32) { return 0; }
VehicleId PhysicsWorld::CreateMotorcycle(const MotorcycleDesc&, const Vec3&, f32) { return 0; }
bool PhysicsWorld::GetVehicleState(VehicleId, VehicleState*) const { return false; }
void PhysicsWorld::RemoveVehicle(VehicleId) {}
void PhysicsWorld::DriveVehicle(VehicleId, f32, f32, f32, f32) {}
bool PhysicsWorld::GetVehicleTransform(VehicleId, Vec3*, f32[4]) const { return false; }
bool PhysicsWorld::GetVehicleWheel(VehicleId, u32, Vec3*, f32[4]) const { return false; }
f32 PhysicsWorld::VehicleForwardSpeed(VehicleId) const { return 0; }
CharacterId PhysicsWorld::CreateCharacter(const Vec3&, f32, f32) { return 0; }
void PhysicsWorld::MoveCharacter(CharacterId, const Vec3&, bool, f32, Vec3*, bool*) {}
void PhysicsWorld::MoveCharacterVelocity(CharacterId, const Vec3&, f32, Vec3*, bool* grounded,
                                         Vec3* ground_velocity) {
  if (grounded) *grounded = false;
  if (ground_velocity) *ground_velocity = Vec3{};
}
void PhysicsWorld::SetCharacterPosition(CharacterId, const Vec3&) {}
bool PhysicsWorld::GetCharacterPosition(CharacterId, Vec3*) const { return false; }
void PhysicsWorld::ConfigureCharacter(CharacterId, f32, f32) {}
bool PhysicsWorld::SetCharacterShape(CharacterId, f32, f32) { return false; }
bool PhysicsWorld::SphereCast(const Vec3&, const Vec3&, f32, f32, RayHit*) const { return false; }
StrandGroomId PhysicsWorld::CreateStrandGroom(const StrandGroomDesc&, const Mat4&) { return 0; }
void PhysicsWorld::SetStrandGroomTransform(StrandGroomId, const Mat4&, f32) {}
void PhysicsWorld::SetStrandGroomWind(StrandGroomId, const Vec3&) {}
u32 PhysicsWorld::StrandGroomPositionCount(StrandGroomId) const { return 0; }
bool PhysicsWorld::GetStrandGroomPositions(StrandGroomId, f32*, u32) const { return false; }
void PhysicsWorld::RemoveStrandGroom(StrandGroomId) {}
ClothId PhysicsWorld::CreateCloth(const ClothDesc&, const Mat4&) { return 0; }
bool PhysicsWorld::SetClothTransform(ClothId, const Mat4&, f32) { return false; }
bool PhysicsWorld::SetClothPinTargets(ClothId, const Vec3*, u32, f32) { return false; }
bool PhysicsWorld::SetClothJointTransforms(ClothId, const Mat4*, u32, bool) { return false; }
void PhysicsWorld::SetClothWind(ClothId, const Vec3&) {}
void PhysicsWorld::SetClothPressure(ClothId, f32) {}
u32 PhysicsWorld::ClothVertexCount(ClothId) const { return 0; }
bool PhysicsWorld::GetClothPositions(ClothId, Vec3*, u32) const { return false; }
void PhysicsWorld::RemoveCloth(ClothId) {}
bool PhysicsWorld::Raycast(const Vec3&, const Vec3&, f32, RayHit*) const { return false; }
bool PhysicsWorld::GetBodyTransform(BodyId, Vec3*, f32[4]) const { return false; }

}  // namespace rx::physics
