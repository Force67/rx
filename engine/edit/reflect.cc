#include "edit/reflect.h"

#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

#include "core/log.h"
#include "scene/components.h"

namespace rx::edit {

namespace detail {

// A component's registration record. Names live in a deque so their c_str()
// pointers (handed out as PropDesc::name / ComponentDesc::name) stay stable as
// more props are added; the PropDesc vector may reallocate freely because
// ComponentDesc::props is recomputed from it on every query.
struct RegEntry {
  std::string name;
  ecs::ComponentId id = 0;
  void (*default_construct)(void*) = nullptr;
  std::deque<std::string> prop_names;
  std::vector<PropDesc> props;
  ComponentDesc desc{};  // rebuilt (props pointer) in Finalize
};

}  // namespace detail

namespace {

using detail::RegEntry;

void RegisterBuiltins();

struct Registry {
  std::mutex mutex;
  std::vector<std::unique_ptr<RegEntry>> entries;
  // Recomputed view handed out by AllComponents(); stable once builtins and any
  // app components are registered.
  std::vector<const ComponentDesc*> view;
  bool builtins_done = false;
};

Registry& TheRegistry() {
  static Registry registry;
  return registry;
}

// Refreshes each entry's ComponentDesc to point at its (possibly reallocated)
// prop vector, and rebuilds the flat view. Caller holds the lock.
void Finalize(Registry& reg) {
  reg.view.clear();
  reg.view.reserve(reg.entries.size());
  for (auto& entry : reg.entries) {
    entry->desc.name = entry->name.c_str();
    entry->desc.id = entry->id;
    entry->desc.props = entry->props.data();
    entry->desc.prop_count = static_cast<u32>(entry->props.size());
    reg.view.push_back(&entry->desc);
  }
}

void EnsureBuiltins() {
  Registry& reg = TheRegistry();
  {
    std::lock_guard<std::mutex> lock(reg.mutex);
    if (reg.builtins_done) return;
    reg.builtins_done = true;  // set first: RegisterBuiltins re-enters via ReflectComponent
  }
  RegisterBuiltins();
  std::lock_guard<std::mutex> lock(reg.mutex);
  Finalize(reg);
}

RegEntry* FindEntry(ecs::ComponentId id) {
  Registry& reg = TheRegistry();
  for (auto& entry : reg.entries)
    if (entry->id == id) return entry.get();
  return nullptr;
}

}  // namespace

namespace detail {

RegEntry* CreateEntry(const char* name, ecs::ComponentId id, void (*default_construct)(void*)) {
  Registry& reg = TheRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  for (auto& entry : reg.entries) {
    if (entry->id == id) return entry.get();  // already registered
  }
  auto entry = std::make_unique<RegEntry>();
  entry->name = name;
  entry->id = id;
  entry->default_construct = default_construct;
  RegEntry* raw = entry.get();
  reg.entries.push_back(std::move(entry));
  return raw;
}

void AddProp(RegEntry* entry, const char* name, PropType type, u32 offset) {
  if (!entry) return;
  Registry& reg = TheRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  entry->prop_names.emplace_back(name);
  entry->props.push_back(PropDesc{entry->prop_names.back().c_str(), type, offset});
}

void SetRange(RegEntry* entry, f32 min, f32 max) {
  if (!entry || entry->props.empty()) return;
  Registry& reg = TheRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  if (entry->props.empty()) return;
  entry->props.back().min = min;
  entry->props.back().max = max;
}

}  // namespace detail

namespace {

void RegisterBuiltins() {
  using namespace scene;
  ReflectComponent<Transform>("Transform")
      .Prop("position", &Transform::position)
      .Prop("rotation", &Transform::rotation, PropType::kQuat)
      .Prop("scale", &Transform::scale)
      .Range(0.0001f, 10000.f);
  ReflectComponent<Renderable>("Renderable").Prop("mesh", &Renderable::mesh);
  ReflectComponent<Hidden>("Hidden");  // tag: no fields
  ReflectComponent<Name>("Name").Prop("value", &Name::value);
  ReflectComponent<Guid>("Guid").Prop("value", &Guid::value);
  ReflectComponent<SpawnedFrom>("SpawnedFrom").Prop("prefab", &SpawnedFrom::prefab);
  ReflectComponent<Parent>("Parent").Prop("value", &Parent::value);
}

}  // namespace

std::span<const ComponentDesc* const> AllComponents() {
  EnsureBuiltins();
  Registry& reg = TheRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  Finalize(reg);  // pick up any components registered after builtins
  return std::span<const ComponentDesc* const>(reg.view.data(), reg.view.size());
}

const ComponentDesc* FindComponent(ecs::ComponentId id) {
  EnsureBuiltins();
  Registry& reg = TheRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  for (auto& entry : reg.entries) {
    if (entry->id == id) {
      entry->desc.props = entry->props.data();
      entry->desc.prop_count = static_cast<u32>(entry->props.size());
      entry->desc.name = entry->name.c_str();
      entry->desc.id = entry->id;
      return &entry->desc;
    }
  }
  return nullptr;
}

const ComponentDesc* FindComponentByName(std::string_view name) {
  EnsureBuiltins();
  Registry& reg = TheRegistry();
  std::lock_guard<std::mutex> lock(reg.mutex);
  for (auto& entry : reg.entries) {
    if (entry->name == name) {
      entry->desc.props = entry->props.data();
      entry->desc.prop_count = static_cast<u32>(entry->props.size());
      entry->desc.name = entry->name.c_str();
      entry->desc.id = entry->id;
      return &entry->desc;
    }
  }
  return nullptr;
}

std::vector<const ComponentDesc*> ComponentsOn(ecs::World& world, ecs::Entity entity) {
  std::vector<const ComponentDesc*> out;
  for (const ComponentDesc* desc : AllComponents()) {
    if (world.HasRaw(entity, desc->id)) out.push_back(desc);
  }
  return out;
}

bool GetProp(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp, const PropDesc& prop,
             PropValue* out) {
  if (!out) return false;
  void* base = world.GetRaw(entity, comp.id);
  if (!base) return false;
  const u8* field = static_cast<const u8*>(base) + prop.offset;
  switch (prop.type) {
    case PropType::kBool: *out = PropValue::Bool(*reinterpret_cast<const bool*>(field)); break;
    case PropType::kI32: *out = PropValue::I32(*reinterpret_cast<const i32*>(field)); break;
    case PropType::kU32: *out = PropValue::U32(*reinterpret_cast<const u32*>(field)); break;
    case PropType::kU64: *out = PropValue::U64(*reinterpret_cast<const u64*>(field)); break;
    case PropType::kF32: *out = PropValue::F32(*reinterpret_cast<const f32*>(field)); break;
    case PropType::kVec2: {
      const f32* v = reinterpret_cast<const f32*>(field);
      *out = PropValue::Vec2(v[0], v[1]);
      break;
    }
    case PropType::kVec3: {
      const f32* v = reinterpret_cast<const f32*>(field);
      *out = PropValue::Vec3(v[0], v[1], v[2]);
      break;
    }
    case PropType::kVec4: {
      const f32* v = reinterpret_cast<const f32*>(field);
      *out = PropValue::Vec4(v[0], v[1], v[2], v[3]);
      break;
    }
    case PropType::kQuat: {
      const f32* v = reinterpret_cast<const f32*>(field);
      *out = PropValue::Quat(v[0], v[1], v[2], v[3]);
      break;
    }
    case PropType::kColor: {
      const f32* v = reinterpret_cast<const f32*>(field);
      *out = PropValue::Color(v[0], v[1], v[2], v[3]);
      break;
    }
    case PropType::kString:
      *out = PropValue::String(*reinterpret_cast<const std::string*>(field));
      break;
    case PropType::kAssetId:
      *out = PropValue::AssetIdV(reinterpret_cast<const asset::AssetId*>(field)->hash);
      break;
    case PropType::kEntity:
      *out = PropValue::EntityV(*reinterpret_cast<const ecs::Entity*>(field));
      break;
  }
  return true;
}

bool SetProp(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp, const PropDesc& prop,
             const PropValue& value) {
  void* base = world.GetRaw(entity, comp.id);
  if (!base) return false;
  u8* field = static_cast<u8*>(base) + prop.offset;
  switch (prop.type) {
    case PropType::kBool: *reinterpret_cast<bool*>(field) = value.b; break;
    case PropType::kI32: *reinterpret_cast<i32*>(field) = static_cast<i32>(value.i); break;
    case PropType::kU32: *reinterpret_cast<u32*>(field) = static_cast<u32>(value.u); break;
    case PropType::kU64: *reinterpret_cast<u64*>(field) = value.u; break;
    case PropType::kF32: *reinterpret_cast<f32*>(field) = value.f[0]; break;
    case PropType::kVec2:
      std::memcpy(field, value.f, 2 * sizeof(f32));
      break;
    case PropType::kVec3:
      std::memcpy(field, value.f, 3 * sizeof(f32));
      break;
    case PropType::kVec4:
    case PropType::kQuat:
    case PropType::kColor:
      std::memcpy(field, value.f, 4 * sizeof(f32));
      break;
    case PropType::kString: *reinterpret_cast<std::string*>(field) = value.s; break;
    case PropType::kAssetId: reinterpret_cast<asset::AssetId*>(field)->hash = value.u; break;
    case PropType::kEntity: *reinterpret_cast<ecs::Entity*>(field) = value.e; break;
  }
  return true;
}

bool AddComponentByDesc(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp) {
  if (!world.IsAlive(entity)) return false;
  RegEntry* entry = nullptr;
  {
    Registry& reg = TheRegistry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    entry = FindEntry(comp.id);
  }
  if (!entry || !entry->default_construct) {
    RX_WARN("edit: cannot add component '{}' (no default constructor registered)", comp.name);
    return false;
  }
  void* slot = world.AddRaw(entity, comp.id);  // raw, possibly re-used memory
  if (!slot) return false;
  entry->default_construct(slot);
  return true;
}

bool RemoveComponentByDesc(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp) {
  if (!world.IsAlive(entity)) return false;
  if (!world.HasRaw(entity, comp.id)) return false;
  world.RemoveRaw(entity, comp.id);
  return true;
}

}  // namespace rx::edit
