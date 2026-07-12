#include "edit/hierarchy.h"

#include <random>

namespace rx::edit {

namespace {

u64 RandomGuid() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  u64 v = 0;
  while (v == 0) v = rng();  // never hand out the 0 sentinel
  return v;
}

Vec3 Pos(const scene::Transform& t) { return {t.position[0], t.position[1], t.position[2]}; }
Quat Rot(const scene::Transform& t) {
  return {t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]};
}

scene::Transform Make(const Vec3& p, const Quat& q, f32 s) {
  scene::Transform t;
  t.position[0] = p.x; t.position[1] = p.y; t.position[2] = p.z;
  t.rotation[0] = q.x; t.rotation[1] = q.y; t.rotation[2] = q.z; t.rotation[3] = q.w;
  t.scale = s;
  return t;
}

}  // namespace

u64 EnsureGuid(ecs::World& world, ecs::Entity entity) {
  if (scene::Guid* g = world.Get<scene::Guid>(entity)) {
    if (g->value == 0) g->value = RandomGuid();
    return g->value;
  }
  u64 value = RandomGuid();
  world.Add(entity, scene::Guid{value});
  return value;
}

ecs::Entity FindByGuid(ecs::World& world, u64 guid) {
  if (guid == 0) return ecs::kInvalidEntity;
  ecs::Entity found = ecs::kInvalidEntity;
  world.Each<scene::Guid>([&](ecs::Entity e, scene::Guid& g) {
    if (g.value == guid) found = e;
  });
  return found;
}

scene::Transform ComposeTransform(const scene::Transform& parent, const scene::Transform& child) {
  // world = parent then child, uniform scale: compose scale, rotate the child's
  // offset by the parent and scale it, chain rotations.
  Quat pq = Rot(parent);
  f32 world_scale = parent.scale * child.scale;
  Quat world_rot = pq * Rot(child);
  Vec3 world_pos = Pos(parent) + Rotate(pq, Pos(child) * parent.scale);
  return Make(world_pos, world_rot, world_scale);
}

scene::Transform WorldTransform(ecs::World& world, ecs::Entity entity) {
  scene::Transform* local = world.Get<scene::Transform>(entity);
  scene::Transform result = local ? *local : scene::Transform{};
  scene::Parent* parent = world.Get<scene::Parent>(entity);
  // Bounded walk: a corrupt cycle would otherwise loop forever.
  int guard = 0;
  while (parent && parent->value && world.IsAlive(parent->value) && guard++ < 4096) {
    ecs::Entity pe = parent->value;
    scene::Transform* pt = world.Get<scene::Transform>(pe);
    result = ComposeTransform(pt ? *pt : scene::Transform{}, result);
    parent = world.Get<scene::Parent>(pe);
  }
  return result;
}

Mat4 LocalMatrix(const scene::Transform& t) {
  return MakeTranslation(Pos(t)) * MakeFromQuat(Rot(t)) * MakeScale(t.scale);
}

Mat4 WorldMatrix(ecs::World& world, ecs::Entity entity) {
  scene::Transform* local = world.Get<scene::Transform>(entity);
  Mat4 m = local ? LocalMatrix(*local) : Mat4::Identity();
  scene::Parent* parent = world.Get<scene::Parent>(entity);
  int guard = 0;
  while (parent && parent->value && world.IsAlive(parent->value) && guard++ < 4096) {
    ecs::Entity pe = parent->value;
    scene::Transform* pt = world.Get<scene::Transform>(pe);
    m = (pt ? LocalMatrix(*pt) : Mat4::Identity()) * m;
    parent = world.Get<scene::Parent>(pe);
  }
  return m;
}

}  // namespace rx::edit
