#ifndef RX_INVENTORY_SERIALIZE_H_
#define RX_INVENTORY_SERIALIZE_H_

#include <vector>

#include "core/types.h"
#include "inventory/export.h"

// Compact, versioned, explicit-little-endian persistence for inventories and
// equipment. A self-contained binary blob is used rather than edit::/.rxscene
// reflection: inventories are frequently-mutated gameplay state saved to a game
// save file, not editor scene documents, and the blob wants to be stable,
// tiny and independent of the editor module's schema. See the module README for
// the rationale.

namespace rx::ecs {
class World;
}

namespace rx::inventory {

// Serializes every entity that has BOTH a scene::Guid and an Inventory (plus
// its Equipment, if present) into a versioned blob, keyed by Guid so the data
// survives ECS handle reuse across a save/load. Entities without a Guid are
// skipped (a scene needs a stable identity to reattach state to).
RX_INVENTORY_EXPORT std::vector<u8> SaveInventories(ecs::World& world);
// Restores a SaveInventories blob. Each record attaches to the live entity
// whose scene::Guid matches; if none exists, a new entity is created carrying
// that Guid. Returns false on a corrupt or unsupported blob.
RX_INVENTORY_EXPORT bool LoadInventories(ecs::World& world, const std::vector<u8>& blob);

}  // namespace rx::inventory

#endif  // RX_INVENTORY_SERIALIZE_H_
