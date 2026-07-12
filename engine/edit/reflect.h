#ifndef RX_EDIT_REFLECT_H_
#define RX_EDIT_REFLECT_H_

#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "asset/asset_id.h"
#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "ecs/component.h"
#include "ecs/entity.h"
#include "ecs/world.h"

// Field-level reflection for ecs components: enough for a generic property
// inspector, a text scene serializer and an undo system to read and write any
// registered component by name without knowing its C++ type. The ecs core
// carries only size/align/move/destruct per component; this layer adds names,
// field offsets and types on top, entirely outside the ecs module.
namespace rx::edit {

enum class PropType : u32 {
  kBool,
  kI32,
  kU32,
  kU64,
  kF32,
  kVec2,
  kVec3,
  kVec4,
  kQuat,
  kColor,
  kString,
  kAssetId,
  kEntity,
};

// A tagged value carrying any reflected field. The active member is selected by
// `type`; the makers below fill exactly the relevant fields.
struct PropValue {
  PropType type = PropType::kF32;
  bool b = false;
  i64 i = 0;
  u64 u = 0;
  f32 f[4] = {};
  std::string s;
  ecs::Entity e{};

  static PropValue Bool(bool v) { PropValue p; p.type = PropType::kBool; p.b = v; return p; }
  static PropValue I32(i32 v) { PropValue p; p.type = PropType::kI32; p.i = v; return p; }
  static PropValue U32(u32 v) { PropValue p; p.type = PropType::kU32; p.u = v; return p; }
  static PropValue U64(u64 v) { PropValue p; p.type = PropType::kU64; p.u = v; return p; }
  static PropValue F32(f32 v) { PropValue p; p.type = PropType::kF32; p.f[0] = v; return p; }
  static PropValue Vec2(f32 x, f32 y) {
    PropValue p; p.type = PropType::kVec2; p.f[0] = x; p.f[1] = y; return p;
  }
  static PropValue Vec3(f32 x, f32 y, f32 z) {
    PropValue p; p.type = PropType::kVec3; p.f[0] = x; p.f[1] = y; p.f[2] = z; return p;
  }
  static PropValue Vec4(f32 x, f32 y, f32 z, f32 w) {
    PropValue p; p.type = PropType::kVec4; p.f[0] = x; p.f[1] = y; p.f[2] = z; p.f[3] = w; return p;
  }
  static PropValue Quat(f32 x, f32 y, f32 z, f32 w) {
    PropValue p; p.type = PropType::kQuat; p.f[0] = x; p.f[1] = y; p.f[2] = z; p.f[3] = w; return p;
  }
  static PropValue Color(f32 r, f32 g, f32 b, f32 a) {
    PropValue p; p.type = PropType::kColor; p.f[0] = r; p.f[1] = g; p.f[2] = b; p.f[3] = a; return p;
  }
  static PropValue String(std::string v) {
    PropValue p; p.type = PropType::kString; p.s = std::move(v); return p;
  }
  static PropValue AssetIdV(u64 hash) { PropValue p; p.type = PropType::kAssetId; p.u = hash; return p; }
  static PropValue EntityV(ecs::Entity v) { PropValue p; p.type = PropType::kEntity; p.e = v; return p; }
};

// One reflected field of a component. `offset` is the byte offset of the field
// within the component struct; `min`/`max` bound a numeric editor (0/0 = free).
struct PropDesc {
  const char* name;
  PropType type;
  u32 offset;
  f32 min = 0.f;
  f32 max = 0.f;
  const char* hint = nullptr;
};

// A reflected component: its display name, its ecs component id and its fields.
struct ComponentDesc {
  const char* name;
  ecs::ComponentId id;
  const PropDesc* props;
  u32 prop_count;
};

namespace detail {

// Opaque per-component registration record; the reflector appends fields to it.
struct RegEntry;

RX_EDIT_EXPORT RegEntry* CreateEntry(const char* name, ecs::ComponentId id,
                                     void (*default_construct)(void*));
RX_EDIT_EXPORT void AddProp(RegEntry* entry, const char* name, PropType type, u32 offset);
RX_EDIT_EXPORT void SetRange(RegEntry* entry, f32 min, f32 max);

// Byte offset of a member within its enclosing struct. Computed against a real
// (default-constructed) instance rather than the null-pointer trick, so it is
// well defined; works for array members too (Transform::position is f32[3]).
template <typename T, typename M>
u32 MemberOffset(M T::* member) {
  static const T probe{};
  return static_cast<u32>(reinterpret_cast<uintptr_t>(&(probe.*member)) -
                          reinterpret_cast<uintptr_t>(&probe));
}

// Maps a member's C++ type to a PropType. Handles the engine math structs, the
// scalar types, std::string, asset::AssetId, ecs::Entity, and raw f32[N] arrays
// (the engine Transform stores position/rotation as arrays).
template <typename M>
constexpr PropType DeducePropType() {
  using T = std::remove_cvref_t<M>;
  if constexpr (std::is_array_v<T>) {
    constexpr size_t n = std::extent_v<T>;
    static_assert(std::is_same_v<std::remove_extent_t<T>, f32>, "only f32[N] arrays are reflectable");
    static_assert(n >= 2 && n <= 4, "only f32[2..4] arrays are reflectable");
    if constexpr (n == 2) return PropType::kVec2;
    else if constexpr (n == 3) return PropType::kVec3;
    else return PropType::kVec4;
  } else if constexpr (std::is_same_v<T, bool>) {
    return PropType::kBool;
  } else if constexpr (std::is_same_v<T, i32>) {
    return PropType::kI32;
  } else if constexpr (std::is_same_v<T, u32>) {
    return PropType::kU32;
  } else if constexpr (std::is_same_v<T, u64>) {
    return PropType::kU64;
  } else if constexpr (std::is_same_v<T, f32>) {
    return PropType::kF32;
  } else if constexpr (std::is_same_v<T, Vec3>) {
    return PropType::kVec3;
  } else if constexpr (std::is_same_v<T, Quat>) {
    return PropType::kQuat;
  } else if constexpr (std::is_same_v<T, std::string>) {
    return PropType::kString;
  } else if constexpr (std::is_same_v<T, asset::AssetId>) {
    return PropType::kAssetId;
  } else if constexpr (std::is_same_v<T, ecs::Entity>) {
    return PropType::kEntity;
  } else {
    static_assert(sizeof(T) == 0, "unreflectable member type");
    return PropType::kF32;
  }
}

}  // namespace detail

// Fluent per-component registration. PropType is deduced from the member type;
// the (name, member, PropType) overload forces a type where the C++ layout is
// ambiguous (a f32[4] that is a quaternion or a color rather than a plain vec4).
template <typename T>
class ComponentReflector {
 public:
  explicit ComponentReflector(detail::RegEntry* entry) : entry_(entry) {}

  template <typename M>
  ComponentReflector& Prop(const char* name, M T::* member) {
    detail::AddProp(entry_, name, detail::DeducePropType<M>(), detail::MemberOffset(member));
    return *this;
  }

  template <typename M>
  ComponentReflector& Prop(const char* name, M T::* member, PropType type) {
    detail::AddProp(entry_, name, type, detail::MemberOffset(member));
    return *this;
  }

  // Applies to the most recently added prop.
  ComponentReflector& Range(f32 min, f32 max) {
    detail::SetRange(entry_, min, max);
    return *this;
  }

 private:
  detail::RegEntry* entry_;
};

// Registers reflection for component T under `name`. Idempotent per T: repeated
// calls return the same reflector (its fields are registered only on the first).
template <typename T>
ComponentReflector<T>& ReflectComponent(const char* name) {
  // Function-local static: the entry is created and its fields registered on the
  // first call for T; later calls return the same reflector. A default
  // constructor is captured when available (AddComponentByDesc needs it); a
  // component without one is still reflectable, just not addable by desc.
  constexpr auto default_ctor = [] {
    if constexpr (std::is_default_constructible_v<T>)
      return +[](void* slot) { new (slot) T(); };
    else
      return static_cast<void (*)(void*)>(nullptr);
  }();
  static ComponentReflector<T> reflector(
      detail::CreateEntry(name, ecs::GetComponentId<T>(), default_ctor));
  return reflector;
}

// Every registered component (builtins are registered on first use).
RX_EDIT_EXPORT std::span<const ComponentDesc* const> AllComponents();
RX_EDIT_EXPORT const ComponentDesc* FindComponent(ecs::ComponentId id);
RX_EDIT_EXPORT const ComponentDesc* FindComponentByName(std::string_view name);
// Reflected components present on `entity`, in registration order.
RX_EDIT_EXPORT std::vector<const ComponentDesc*> ComponentsOn(ecs::World& world, ecs::Entity entity);

// Reads/writes a single field. False if the component is absent on the entity.
RX_EDIT_EXPORT bool GetProp(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp,
                            const PropDesc& prop, PropValue* out);
RX_EDIT_EXPORT bool SetProp(ecs::World& world, ecs::Entity entity, const ComponentDesc& comp,
                            const PropDesc& prop, const PropValue& value);

// Adds a default-constructed component (no-op-safe if already present: it is
// re-default-constructed). Removes a component. False on a bad entity/desc.
RX_EDIT_EXPORT bool AddComponentByDesc(ecs::World& world, ecs::Entity entity,
                                       const ComponentDesc& comp);
RX_EDIT_EXPORT bool RemoveComponentByDesc(ecs::World& world, ecs::Entity entity,
                                          const ComponentDesc& comp);

}  // namespace rx::edit

#endif  // RX_EDIT_REFLECT_H_
