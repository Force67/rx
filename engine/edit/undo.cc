#include "edit/undo.h"

#include "base/containers/pair.h"
#include "base/containers/vector.h"
#include "base/external/xoshiro256ss/xoshiro256ss.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/random/random.h"
#include "base/strings/xstring.h"
#include "edit/hierarchy.h"
#include "foundation/math/math.h"
#include "foundation/strings/format.h"
#include "scene/components.h"

namespace rx::edit {
namespace {

u64 RandomGuid() {
  // Guids only need to be unique, not reproducible: a per-thread 64-bit
  // generator seeded once from the OS keeps every draw full width without a
  // syscall per id.
  static thread_local base::xoshiro256ss rng{base::SourceTrueRandomSeed()};
  u64 v = 0;
  while (v == 0)
    v = rng();
  return v;
}

// A full snapshot of one component instance: its desc plus each prop's value.
// kEntity props are stored by the referenced entity's guid (in PropValue.u) so
// they survive the target being destroyed and recreated.
struct CompSnap {
  const ComponentDesc *comp;
  base::Vector<base::Pair<const PropDesc *, PropValue>> props;
};

base::Vector<CompSnap> SnapshotEntity(ecs::World &world, ecs::Entity entity) {
  base::Vector<CompSnap> out;
  for (const ComponentDesc *comp : ComponentsOn(world, entity)) {
    CompSnap snap{comp, {}};
    for (u32 i = 0; i < comp->prop_count; ++i) {
      const PropDesc &prop = comp->props[i];
      PropValue v;
      if (!GetProp(world, entity, *comp, prop, &v))
        continue;
      if (prop.type == PropType::kEntity) {
        v.u = (v.e && world.IsAlive(v.e)) ? EnsureGuid(world, v.e)
                                          : 0; // stash target guid
      }
      snap.props.emplace_back(&prop, base::move(v));
    }
    out.push_back(base::move(snap));
  }
  return out;
}

void RestoreComponents(ecs::World &world, ecs::Entity entity,
                       const base::Vector<CompSnap> &snaps) {
  for (const CompSnap &snap : snaps) {
    AddComponentByDesc(world, entity, *snap.comp);
    for (const auto &[prop, value] : snap.props) {
      PropValue v = value;
      if (prop->type == PropType::kEntity)
        v.e = v.u ? FindByGuid(world, v.u) : ecs::kInvalidEntity;
      SetProp(world, entity, *snap.comp, *prop, v);
    }
  }
}

scene::Transform TransformFromMatrix(const Mat4 &m) {
  Vec3 pos = Translation(m);
  Quat q = QuatFromMat4(m);
  f32 scale = Length(Vec3{m.m[0], m.m[1], m.m[2]});
  scene::Transform t;
  t.position[0] = pos.x;
  t.position[1] = pos.y;
  t.position[2] = pos.z;
  t.rotation[0] = q.x;
  t.rotation[1] = q.y;
  t.rotation[2] = q.z;
  t.rotation[3] = q.w;
  t.scale = scale;
  return t;
}

void SetTransform(ecs::World &world, ecs::Entity e, const scene::Transform &t) {
  if (scene::Transform *cur = world.Get<scene::Transform>(e))
    *cur = t;
  else
    world.Add(e, t);
}

// Commands

class SetPropCommand : public Command {
public:
  SetPropCommand(ecs::World &world, ecs::Entity entity,
                 const ComponentDesc &comp, const PropDesc &prop,
                 PropValue new_value)
      : comp_(&comp), prop_(&prop), new_value_(base::move(new_value)) {
    guid_ = EnsureGuid(world, entity);
    GetProp(world, entity, comp, prop, &old_value_);
    label_ = rx::StrFormat("Set {}.{}", comp.name, prop.name);
  }
  void Apply(ecs::World &world) override {
    if (ecs::Entity e = FindByGuid(world, guid_))
      SetProp(world, e, *comp_, *prop_, new_value_);
  }
  void Revert(ecs::World &world) override {
    if (ecs::Entity e = FindByGuid(world, guid_))
      SetProp(world, e, *comp_, *prop_, old_value_);
  }
  const char *label() const override { return label_.c_str(); }

private:
  u64 guid_;
  const ComponentDesc *comp_;
  const PropDesc *prop_;
  PropValue old_value_;
  PropValue new_value_;
  base::String label_;
};

class CreateEntityCommand : public Command {
public:
  CreateEntityCommand(
      base::Vector<
          base::Pair<const ComponentDesc *,
                    base::Vector<base::Pair<const PropDesc *, PropValue>>>>
          initial,
      ecs::Entity *out)
      : initial_(base::move(initial)), out_(out), guid_(RandomGuid()) {}

  void Apply(ecs::World &world) override {
    ecs::Entity e = world.Create();
    world.Add(e, scene::Guid{guid_});
    for (const auto &[comp, props] : initial_) {
      AddComponentByDesc(world, e, *comp);
      for (const auto &[prop, value] : props)
        SetProp(world, e, *comp, *prop, value);
    }
    // Only the first Apply (which runs inside UndoStack::Push, while the
    // caller's pointer is still valid) reports the handle. Callers routinely
    // pass a stack address, so the command must never write through it again
    // on later undo/redo.
    if (out_) {
      *out_ = e;
      out_ = nullptr;
    }
  }
  void Revert(ecs::World &world) override {
    if (ecs::Entity e = FindByGuid(world, guid_))
      world.Destroy(e);
  }
  const char *label() const override { return "Create entity"; }

private:
  base::Vector<base::Pair<const ComponentDesc *,
                        base::Vector<base::Pair<const PropDesc *, PropValue>>>>
      initial_;
  ecs::Entity *out_;
  u64 guid_;
};

class DestroyEntityCommand : public Command {
public:
  DestroyEntityCommand(ecs::World &world, ecs::Entity entity) {
    guid_ = EnsureGuid(world, entity);
    snapshot_ = SnapshotEntity(world, entity);
  }
  void Apply(ecs::World &world) override {
    if (ecs::Entity e = FindByGuid(world, guid_))
      world.Destroy(e);
  }
  void Revert(ecs::World &world) override {
    ecs::Entity e = world.Create();
    RestoreComponents(world, e,
                      snapshot_); // re-adds Guid{guid_} among the rest
  }
  const char *label() const override { return "Destroy entity"; }

private:
  u64 guid_;
  base::Vector<CompSnap> snapshot_;
};

class ReparentCommand : public Command {
public:
  ReparentCommand(ecs::World &world, ecs::Entity entity,
                  ecs::Entity new_parent) {
    guid_ = EnsureGuid(world, entity);
    Mat4 world_matrix = WorldMatrix(world, entity);

    if (scene::Transform *t = world.Get<scene::Transform>(entity))
      old_local_ = *t;
    if (scene::Parent *p = world.Get<scene::Parent>(entity);
        p && p->value && world.IsAlive(p->value)) {
      old_parent_guid_ = EnsureGuid(world, p->value);
    }

    if (new_parent && world.IsAlive(new_parent)) {
      new_parent_guid_ = EnsureGuid(world, new_parent);
      Mat4 parent_world = WorldMatrix(world, new_parent);
      new_local_ = TransformFromMatrix(Inverse(parent_world) * world_matrix);
    } else {
      new_parent_guid_ = 0;
      new_local_ = TransformFromMatrix(world_matrix);
    }
  }
  void Apply(ecs::World &world) override {
    SetLink(world, new_parent_guid_, new_local_);
  }
  void Revert(ecs::World &world) override {
    SetLink(world, old_parent_guid_, old_local_);
  }
  const char *label() const override { return "Reparent"; }

private:
  void SetLink(ecs::World &world, u64 parent_guid,
               const scene::Transform &local) {
    ecs::Entity e = FindByGuid(world, guid_);
    if (!e)
      return;
    SetTransform(world, e, local);
    if (parent_guid != 0) {
      ecs::Entity parent = FindByGuid(world, parent_guid);
      if (scene::Parent *p = world.Get<scene::Parent>(e))
        p->value = parent;
      else
        world.Add(e, scene::Parent{parent});
    } else if (world.Has<scene::Parent>(e)) {
      world.Remove<scene::Parent>(e);
    }
  }

  u64 guid_;
  u64 old_parent_guid_ = 0;
  u64 new_parent_guid_ = 0;
  scene::Transform old_local_;
  scene::Transform new_local_;
};

class AddComponentCommand : public Command {
public:
  AddComponentCommand(ecs::World &world, ecs::Entity entity,
                      const ComponentDesc &comp)
      : comp_(&comp) {
    guid_ = EnsureGuid(world, entity);
    existed_ = world.HasRaw(entity, comp.id);
    label_ = rx::StrFormat("Add {}", comp.name);
  }
  void Apply(ecs::World &world) override {
    if (existed_)
      return;
    if (ecs::Entity e = FindByGuid(world, guid_))
      AddComponentByDesc(world, e, *comp_);
  }
  void Revert(ecs::World &world) override {
    if (existed_)
      return;
    if (ecs::Entity e = FindByGuid(world, guid_))
      RemoveComponentByDesc(world, e, *comp_);
  }
  const char *label() const override { return label_.c_str(); }

private:
  u64 guid_;
  const ComponentDesc *comp_;
  bool existed_;
  base::String label_;
};

class RemoveComponentCommand : public Command {
public:
  RemoveComponentCommand(ecs::World &world, ecs::Entity entity,
                         const ComponentDesc &comp)
      : comp_(&comp) {
    guid_ = EnsureGuid(world, entity);
    existed_ = world.HasRaw(entity, comp.id);
    if (existed_) {
      for (u32 i = 0; i < comp.prop_count; ++i) {
        const PropDesc &prop = comp.props[i];
        PropValue v;
        if (!GetProp(world, entity, comp, prop, &v))
          continue;
        if (prop.type == PropType::kEntity)
          v.u = (v.e && world.IsAlive(v.e)) ? EnsureGuid(world, v.e) : 0;
        props_.emplace_back(&prop, base::move(v));
      }
    }
    label_ = rx::StrFormat("Remove {}", comp.name);
  }
  void Apply(ecs::World &world) override {
    if (!existed_)
      return;
    if (ecs::Entity e = FindByGuid(world, guid_))
      RemoveComponentByDesc(world, e, *comp_);
  }
  void Revert(ecs::World &world) override {
    if (!existed_)
      return;
    ecs::Entity e = FindByGuid(world, guid_);
    if (!e)
      return;
    AddComponentByDesc(world, e, *comp_);
    for (const auto &[prop, value] : props_) {
      PropValue v = value;
      if (prop->type == PropType::kEntity)
        v.e = v.u ? FindByGuid(world, v.u) : ecs::kInvalidEntity;
      SetProp(world, e, *comp_, *prop, v);
    }
  }
  const char *label() const override { return label_.c_str(); }

private:
  u64 guid_;
  const ComponentDesc *comp_;
  bool existed_;
  base::Vector<base::Pair<const PropDesc *, PropValue>> props_;
  base::String label_;
};

class CompositeCommand : public Command {
public:
  CompositeCommand(base::Vector<base::UniquePointer<Command>> children,
                   base::String label)
      : children_(base::move(children)), label_(base::move(label)) {}
  void Apply(ecs::World &world) override {
    for (auto &c : children_)
      c->Apply(world);
  }
  void Revert(ecs::World &world) override {
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
      (*it)->Revert(world);
  }
  const char *label() const override { return label_.c_str(); }

private:
  base::Vector<base::UniquePointer<Command>> children_;
  base::String label_;
};

} // namespace

void UndoStack::Push(ecs::World &world, base::UniquePointer<Command> cmd) {
  if (!cmd)
    return;
  cmd->Apply(world);
  RecordApplied(base::move(cmd));
}

void UndoStack::RecordApplied(base::UniquePointer<Command> cmd) {
  if (!cmd)
    return;
  if (group_depth_ > 0) {
    group_buffer_.push_back(base::move(cmd));
  } else {
    undo_.push_back(base::move(cmd));
    redo_.clear();
  }
}

bool UndoStack::Undo(ecs::World &world) {
  if (undo_.empty())
    return false;
  base::UniquePointer<Command> cmd = base::move(undo_.back());
  undo_.pop_back();
  cmd->Revert(world);
  redo_.push_back(base::move(cmd));
  return true;
}

bool UndoStack::Redo(ecs::World &world) {
  if (redo_.empty())
    return false;
  base::UniquePointer<Command> cmd = base::move(redo_.back());
  redo_.pop_back();
  cmd->Apply(world);
  undo_.push_back(base::move(cmd));
  return true;
}

void UndoStack::BeginGroup(const char *label) {
  if (group_depth_++ == 0) {
    group_label_ = label ? label : "";
    group_buffer_.clear();
  }
}

void UndoStack::EndGroup() {
  if (group_depth_ == 0)
    return;
  if (--group_depth_ == 0 && !group_buffer_.empty()) {
    undo_.push_back(base::MakeUnique<CompositeCommand>(base::move(group_buffer_),
                                                       group_label_));
    group_buffer_.clear();
    redo_.clear();
  }
}

void UndoStack::Clear() {
  undo_.clear();
  redo_.clear();
  group_buffer_.clear();
  group_depth_ = 0;
}

// Factories

base::UniquePointer<Command> MakeSetProp(ecs::World &world, ecs::Entity entity,
                                     const ComponentDesc &comp,
                                     const PropDesc &prop,
                                     PropValue new_value) {
  return base::MakeUnique<SetPropCommand>(world, entity, comp, prop,
                                          base::move(new_value));
}

base::UniquePointer<Command> MakeCreateEntity(
    base::Vector<base::Pair<const ComponentDesc *,
                          base::Vector<base::Pair<const PropDesc *, PropValue>>>>
        initial,
    ecs::Entity *out_entity) {
  return base::MakeUnique<CreateEntityCommand>(base::move(initial), out_entity);
}

base::UniquePointer<Command> MakeDestroyEntity(ecs::World &world,
                                           ecs::Entity entity) {
  return base::MakeUnique<DestroyEntityCommand>(world, entity);
}

base::UniquePointer<Command> MakeReparent(ecs::World &world, ecs::Entity entity,
                                      ecs::Entity new_parent) {
  return base::MakeUnique<ReparentCommand>(world, entity, new_parent);
}

base::UniquePointer<Command> MakeAddComponent(ecs::World &world, ecs::Entity entity,
                                          const ComponentDesc &comp) {
  return base::MakeUnique<AddComponentCommand>(world, entity, comp);
}

base::UniquePointer<Command> MakeRemoveComponent(ecs::World &world,
                                             ecs::Entity entity,
                                             const ComponentDesc &comp) {
  return base::MakeUnique<RemoveComponentCommand>(world, entity, comp);
}

} // namespace rx::edit
