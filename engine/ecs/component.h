#ifndef RX_ECS_COMPONENT_H_
#define RX_ECS_COMPONENT_H_

#include <type_traits>

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

RX_ECS_EXPORT ComponentId NextComponentId();
RX_ECS_EXPORT void RegisterComponent(ComponentId id, const ComponentInfo& info);

// RX_DSO_EXPORT (default visibility, even under -fvisibility-inlines-hidden)
// forces the function-local `id` static below to be a single process-wide
// instance. Without it each DSO in an RX_SHARED build would instantiate its own
// copy, call NextComponentId() independently, and hand the SAME component type a
// DIFFERENT id in different shared objects. No-op in the static build.
template <typename T>
RX_DSO_EXPORT ComponentId ComponentIdFor() {
  static_assert(std::is_move_constructible_v<T>);
  static const ComponentId id = [] {
    ComponentId new_id = NextComponentId();
    RegisterComponent(new_id, ComponentInfo{
      .size = sizeof(T),
      .align = alignof(T),
      .move_construct = [](void* dst, void* src) { new (dst) T(std::move(*static_cast<T*>(src))); },
      .destruct = [](void* ptr) { static_cast<T*>(ptr)->~T(); },
    });
    return new_id;
  }();
  return id;
}

}  // namespace detail

template <typename T>
RX_DSO_EXPORT ComponentId GetComponentId() {
  return detail::ComponentIdFor<std::remove_cvref_t<T>>();
}

RX_ECS_EXPORT const ComponentInfo& GetComponentInfo(ComponentId id);

}  // namespace rx::ecs

#endif  // RX_ECS_COMPONENT_H_
