#ifndef RX_EDIT_HIERARCHY_H_
#define RX_EDIT_HIERARCHY_H_

#include "core/export.h"
#include "core/math.h"
#include "core/types.h"
#include "ecs/entity.h"
#include "ecs/world.h"
#include "scene/components.h"

// Identity and parent-chain helpers shared by the scene serializer, the undo
// system and the render-time gather. Kept out of the header-only scene module so
// they can carry logic and depend on the ecs World.
namespace rx::edit {

// Assigns a random nonzero Guid if the entity has none; returns the value. A
// second call is a no-op (returns the existing value). Nonzero is guaranteed so
// 0 stays a valid "no reference" sentinel.
RX_EDIT_EXPORT u64 EnsureGuid(ecs::World& world, ecs::Entity entity);

// The entity carrying `guid`, or kInvalidEntity. Linear scan over Guid
// components (editor scale, not a hot path).
RX_EDIT_EXPORT ecs::Entity FindByGuid(ecs::World& world, u64 guid);

// Composes a single parent*child step of uniform-scale TRS transforms.
RX_EDIT_EXPORT scene::Transform ComposeTransform(const scene::Transform& parent,
                                                 const scene::Transform& child);

// The world-space Transform of `entity`, composing its local Transform up the
// Parent chain. An entity with no Parent returns its own Transform unchanged.
RX_EDIT_EXPORT scene::Transform WorldTransform(ecs::World& world, ecs::Entity entity);

// The world-space model matrix of `entity` (Parent chain composed as matrices).
RX_EDIT_EXPORT Mat4 WorldMatrix(ecs::World& world, ecs::Entity entity);

// The column-major model matrix of a single Transform (translate * rotate *
// uniform scale), matching the engine's gather convention.
RX_EDIT_EXPORT Mat4 LocalMatrix(const scene::Transform& t);

}  // namespace rx::edit

#endif  // RX_EDIT_HIERARCHY_H_
