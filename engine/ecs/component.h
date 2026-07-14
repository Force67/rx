#ifndef RX_ECS_COMPONENT_H_
#define RX_ECS_COMPONENT_H_

#include <type_traits>

#include <base/hashing/fnv1a.h>

#include "core/export.h"
#include "core/types.h"

namespace rx::ecs {

using ComponentId = u32;

struct ComponentInfo {
  u32 size = 0;
  u32 align = 0;
  void (*move_construct)(void* dst, void* src) = nullptr;
  void (*destruct)(void* ptr) = nullptr;
};

namespace detail {

RX_ECS_EXPORT ComponentId ResolveComponentId(u64 type_key, const ComponentInfo& info);

// Component types used across translation units must be externally named and
// declared in a shared header. Anonymous-namespace and function-local types are
// safe only when confined to one translation unit: duplicate spellings in
// separate translation units would alias the same compiler-generated key.
template <typename T>
consteval u64 ComponentTypeKey() {
#if defined(_MSC_VER)
  return base::FNV1a64(__FUNCSIG__);
#else
  return base::FNV1a64(__PRETTY_FUNCTION__);
#endif
}

template <typename T>
ComponentId ComponentIdFor() {
  static_assert(std::is_move_constructible_v<T>);
  static const ComponentId id = [] {
    return ResolveComponentId(
        ComponentTypeKey<T>(),
        ComponentInfo{
            .size = sizeof(T),
            .align = alignof(T),
            .move_construct = [](void* dst,
                                 void* src) { new (dst) T(std::move(*static_cast<T*>(src))); },
            .destruct = [](void* ptr) { static_cast<T*>(ptr)->~T(); },
        });
  }();
  return id;
}

}  // namespace detail

template <typename T>
ComponentId GetComponentId() {
  return detail::ComponentIdFor<std::remove_cvref_t<T>>();
}

RX_ECS_EXPORT const ComponentInfo& GetComponentInfo(ComponentId id);

}  // namespace rx::ecs

#endif  // RX_ECS_COMPONENT_H_
