#include "edit/undo.h"

#include <format>
#include <random>

#include "core/math.h"
#include "edit/hierarchy.h"
#include "scene/components.h"

namespace rx::edit {
namespace {

u64 RandomGuid() {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  u64 v = 0;
  while (v == 0) v = rng();
  return v;
}

// A full snapshot of one component instance: its desc plus each prop's value.
// kEntity props are stored by the referenced entity's guid (in PropValue.u) so
// they survive the target being destroyed and recreated.
struct CompSnap {
  const ComponentDesc* comp;
  std::vector<std::pair<const PropDesc*, PropValue>> props;
};

std::vector<CompSnap> SnapshotEntity(ecs::World& world, ecs::Entity entity) {
  std::vector<CompSnap> out;
  for (const ComponentDesc* comp : ComponentsOn(world, entity)) {
    CompSnap snap{comp, {}};
    for (u32 i = 0; i < comp->prop_count; ++i) {
      const PropDesc& prop = comp->props[i];
      PropValue v;
      if (!GetProp(world, entity, *comp, prop, &v)) continue;
      if (prop.type == PropType::kEntity) {
        v.u = (v.e && world.IsAlive(v.e)) ? EnsureGuid(world, v.e) : 0;  // stash target guid
      }
      snap.props.emplace_back(&prop, std::move(v));
    }
    out.push_back(std::move(snap));
  }
  return out;
}

void RestoreComponents(ecs::World& world, ecs::Entity entity, const std::vector<CompSnap>& snaps) {
  for (const CompSnap& snap : snaps) {
    AddComponentByDesc(world, entity, *snap.comp);
    for (const auto& [prop, value] : snap.props) {
      PropValue v = value;
      if (prop->type == PropType::kEntity) v.e = v.u ? FindByGuid(world, v.u) : ecs::kInvalidEntity;
      SetProp(world, entity, *snap.comp, *prop, v);
    }
  }
}

scene::Transform TransformFromMatrix(const Mat4& m) {
  Vec3 pos = Translation(m);
  Quat q = QuatFromMat4(m);
  f32 scale = Length(Vec3{m.m[0], m.m[1], m.m[2]});
  scene::Transform t;
  t.position[0] = pos.x; t.position[1] = pos.y; t.position[2] = pos.z;
  t.rotation[0] = q.x; t.rotation[1] = q.y; t.rotation[2] = q.z; t.rotation[3] = q.w;
  t.scale = scale;
  return t;
}

void SetTransform(ecs::World& world, ecs::Entity e, const scene::Transform& t) {
  if (scene::Transform* cur = world.Get<scene::Transform>(e)) *cur = t;
  else world.Add(e, t);
}

// ---- Commands ---------------------------------------------------------------

class SetPropCommand : public Command {
 public:
  SetPropCommand(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp,
                 const PropDesc& prop, PropValue new_value)
      : comp_(&comp), prop_(&prop), new_value_(std::move(new_value)) {
    guid_ = EnsureGuid(world, entity);
    GetProp(world, entity, comp, prop, &old_value_);
    label_ = std::format("Set {}.{}", comp.name, prop.name);
  }
  void Apply(ecs::World& world) override {
    if (ecs::Entity e = FindByGuid(world, guid_)) SetProp(world, e, *comp_, *prop_, new_value_);
  }
  void Revert(ecs::World& world) override {
    if (ecs::Entity e = FindByGuid(world, guid_)) SetProp(world, e, *comp_, *prop_, old_value_);
  }
  const char* label() const override { return label_.c_str(); }

 private:
  u64 guid_;
  const ComponentDesc* comp_;
  const PropDesc* prop_;
  PropValue old_value_;
  PropValue new_value_;
  std::string label_;
};

class CreateEntityCommand : public Command {
 public:
  CreateEntityCommand(
      std::vector<std::pair<const ComponentDesc*, std::vector<std::pair<const PropDesc*, PropValue>>>>
          initial,
      ecs::Entity* out)
      : initial_(std::move(initial)), out_(out), guid_(RandomGuid()) {}

  void Apply(ecs::World& world) override {
    ecs::Entity e = world.Create();
    world.Add(e, scene::Guid{guid_});
    for (const auto& [comp, props] : initial_) {
      AddComponentByDesc(world, e, *comp);
      for (const auto& [prop, value] : props) SetProp(world, e, *comp, *prop, value);
    }
    if (out_) *out_ = e;
  }
  void Revert(ecs::World& world) override {
    if (ecs::Entity e = FindByGuid(world, guid_)) world.Destroy(e);
    if (out_) *out_ = ecs::kInvalidEntity;
  }
  const char* label() const override { return "Create entity"; }

 private:
  std::vector<std::pair<const ComponentDesc*, std::vector<std::pair<const PropDesc*, PropValue>>>>
      initial_;
  ecs::Entity* out_;
  u64 guid_;
};

class DestroyEntityCommand : public Command {
 public:
  DestroyEntityCommand(ecs::World& world, ecs::Entity entity) {
    guid_ = EnsureGuid(world, entity);
    snapshot_ = SnapshotEntity(world, entity);
  }
  void Apply(ecs::World& world) override {
    if (ecs::Entity e = FindByGuid(world, guid_)) world.Destroy(e);
  }
  void Revert(ecs::World& world) override {
    ecs::Entity e = world.Create();
    RestoreComponents(world, e, snapshot_);  // re-adds Guid{guid_} among the rest
  }
  const char* label() const override { return "Destroy entity"; }

 private:
  u64 guid_;
  std::vector<CompSnap> snapshot_;
};

class ReparentCommand : public Command {
 public:
  ReparentCommand(ecs::World& world, ecs::Entity entity, ecs::Entity new_parent) {
    guid_ = EnsureGuid(world, entity);
    Mat4 world_matrix = WorldMatrix(world, entity);

    if (scene::Transform* t = world.Get<scene::Transform>(entity)) old_local_ = *t;
    if (scene::Parent* p = world.Get<scene::Parent>(entity); p && p->value && world.IsAlive(p->value)) {
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
  void Apply(ecs::World& world) override { SetLink(world, new_parent_guid_, new_local_); }
  void Revert(ecs::World& world) override { SetLink(world, old_parent_guid_, old_local_); }
  const char* label() const override { return "Reparent"; }

 private:
  void SetLink(ecs::World& world, u64 parent_guid, const scene::Transform& local) {
    ecs::Entity e = FindByGuid(world, guid_);
    if (!e) return;
    SetTransform(world, e, local);
    if (parent_guid != 0) {
      ecs::Entity parent = FindByGuid(world, parent_guid);
      if (scene::Parent* p = world.Get<scene::Parent>(e)) p->value = parent;
      else world.Add(e, scene::Parent{parent});
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
  AddComponentCommand(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp)
      : comp_(&comp) {
    guid_ = EnsureGuid(world, entity);
    existed_ = world.HasRaw(entity, comp.id);
    label_ = std::format("Add {}", comp.name);
  }
  void Apply(ecs::World& world) override {
    if (existed_) return;
    if (ecs::Entity e = FindByGuid(world, guid_)) AddComponentByDesc(world, e, *comp_);
  }
  void Revert(ecs::World& world) override {
    if (existed_) return;
    if (ecs::Entity e = FindByGuid(world, guid_)) RemoveComponentByDesc(world, e, *comp_);
  }
  const char* label() const override { return label_.c_str(); }

 private:
  u64 guid_;
  const ComponentDesc* comp_;
  bool existed_;
  std::string label_;
};

class RemoveComponentCommand : public Command {
 public:
  RemoveComponentCommand(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp)
      : comp_(&comp) {
    guid_ = EnsureGuid(world, entity);
    existed_ = world.HasRaw(entity, comp.id);
    if (existed_) {
      for (u32 i = 0; i < comp.prop_count; ++i) {
        const PropDesc& prop = comp.props[i];
        PropValue v;
        if (!GetProp(world, entity, comp, prop, &v)) continue;
        if (prop.type == PropType::kEntity)
          v.u = (v.e && world.IsAlive(v.e)) ? EnsureGuid(world, v.e) : 0;
        props_.emplace_back(&prop, std::move(v));
      }
    }
    label_ = std::format("Remove {}", comp.name);
  }
  void Apply(ecs::World& world) override {
    if (!existed_) return;
    if (ecs::Entity e = FindByGuid(world, guid_)) RemoveComponentByDesc(world, e, *comp_);
  }
  void Revert(ecs::World& world) override {
    if (!existed_) return;
    ecs::Entity e = FindByGuid(world, guid_);
    if (!e) return;
    AddComponentByDesc(world, e, *comp_);
    for (const auto& [prop, value] : props_) {
      PropValue v = value;
      if (prop->type == PropType::kEntity) v.e = v.u ? FindByGuid(world, v.u) : ecs::kInvalidEntity;
      SetProp(world, e, *comp_, *prop, v);
    }
  }
  const char* label() const override { return label_.c_str(); }

 private:
  u64 guid_;
  const ComponentDesc* comp_;
  bool existed_;
  std::vector<std::pair<const PropDesc*, PropValue>> props_;
  std::string label_;
};

class CompositeCommand : public Command {
 public:
  CompositeCommand(std::vector<std::unique_ptr<Command>> children, std::string label)
      : children_(std::move(children)), label_(std::move(label)) {}
  void Apply(ecs::World& world) override {
    for (auto& c : children_) c->Apply(world);
  }
  void Revert(ecs::World& world) override {
    for (auto it = children_.rbegin(); it != children_.rend(); ++it) (*it)->Revert(world);
  }
  const char* label() const override { return label_.c_str(); }

 private:
  std::vector<std::unique_ptr<Command>> children_;
  std::string label_;
};

}  // namespace

// ---- UndoStack --------------------------------------------------------------

void UndoStack::Push(ecs::World& world, std::unique_ptr<Command> cmd) {
  if (!cmd) return;
  cmd->Apply(world);
  if (group_depth_ > 0) {
    group_buffer_.push_back(std::move(cmd));
  } else {
    undo_.push_back(std::move(cmd));
    redo_.clear();
  }
}

bool UndoStack::Undo(ecs::World& world) {
  if (undo_.empty()) return false;
  std::unique_ptr<Command> cmd = std::move(undo_.back());
  undo_.pop_back();
  cmd->Revert(world);
  redo_.push_back(std::move(cmd));
  return true;
}

bool UndoStack::Redo(ecs::World& world) {
  if (redo_.empty()) return false;
  std::unique_ptr<Command> cmd = std::move(redo_.back());
  redo_.pop_back();
  cmd->Apply(world);
  undo_.push_back(std::move(cmd));
  return true;
}

void UndoStack::BeginGroup(const char* label) {
  if (group_depth_++ == 0) {
    group_label_ = label ? label : "";
    group_buffer_.clear();
  }
}

void UndoStack::EndGroup() {
  if (group_depth_ == 0) return;
  if (--group_depth_ == 0 && !group_buffer_.empty()) {
    undo_.push_back(std::make_unique<CompositeCommand>(std::move(group_buffer_), group_label_));
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

// ---- Factories --------------------------------------------------------------

std::unique_ptr<Command> MakeSetProp(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp,
                                     const PropDesc& prop, PropValue new_value) {
  return std::make_unique<SetPropCommand>(world, entity, comp, prop, std::move(new_value));
}

std::unique_ptr<Command> MakeCreateEntity(
    std::vector<std::pair<const ComponentDesc*, std::vector<std::pair<const PropDesc*, PropValue>>>>
        initial,
    ecs::Entity* out_entity) {
  return std::make_unique<CreateEntityCommand>(std::move(initial), out_entity);
}

std::unique_ptr<Command> MakeDestroyEntity(ecs::World& world, ecs::Entity entity) {
  return std::make_unique<DestroyEntityCommand>(world, entity);
}

std::unique_ptr<Command> MakeReparent(ecs::World& world, ecs::Entity entity, ecs::Entity new_parent) {
  return std::make_unique<ReparentCommand>(world, entity, new_parent);
}

std::unique_ptr<Command> MakeAddComponent(ecs::World& world, ecs::Entity entity,
                                          const ComponentDesc& comp) {
  return std::make_unique<AddComponentCommand>(world, entity, comp);
}

std::unique_ptr<Command> MakeRemoveComponent(ecs::World& world, ecs::Entity entity,
                                             const ComponentDesc& comp) {
  return std::make_unique<RemoveComponentCommand>(world, entity, comp);
}

}  // namespace rx::edit
