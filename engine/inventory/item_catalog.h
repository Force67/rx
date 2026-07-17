#ifndef RX_INVENTORY_ITEM_CATALOG_H_
#define RX_INVENTORY_ITEM_CATALOG_H_

#include <unordered_map>

#include "asset/asset_id.h"
#include "core/types.h"
#include "inventory/export.h"
#include "physics/shape_desc.h"

namespace rx::inventory {

// Game-assigned identity for an item definition. 0 means "no item"; every real
// definition is >= 1. Ids are the key persisted in inventories and dropped
// world items, so a game that wants save-file stability should assign explicit
// ids (Register(id, def)) from its own data rather than relying on registration
// order.
using ItemDefId = u32;
constexpr ItemDefId kInvalidItemDef = 0;

// Immutable, game-registered description of a kind of item. The engine assigns
// NO semantics to the taxonomy: name_hash, flags and payload are whatever the
// game wants (a hashed editor string, a bitset of game concepts, a handle into
// a game database). Only the physics/render fields are read by the engine, and
// only when an item is dropped into the world.
struct ItemDef {
  u64 name_hash = 0;          // game string hash (display / debug / identity)
  asset::AssetId world_mesh;  // renderable attached when dropped; 0 => game attaches visuals
  physics::ShapeDesc shape;   // collision shape for the dropped rigid body
  f32 mass = 1.0f;            // kg, for the dropped dynamic body
  f32 friction = 0.5f;
  f32 restitution = 0.0f;
  f32 scale = 1.0f;   // shape-unit -> metre scale handed to PhysicsWorld
  u32 max_stack = 1;  // an inventory entry holds at most this many; 0 == 1
  f32 weight = 0.0f;  // per-unit, counted against Inventory::max_weight
  u32 flags = 0;      // game-defined bits
  u64 payload = 0;    // game-defined per-definition data
};

// A game-owned table of ItemDefs, passed explicitly to the systems that need
// definition data. There is no global catalog: a game may keep several (per
// mod, per save slot) and choose which to hand a given operation.
class RX_INVENTORY_EXPORT ItemCatalog {
 public:
  // Registers `def` under a freshly minted id and returns it.
  ItemDefId Register(const ItemDef& def);
  // Registers `def` under an explicit id (for save-stable / data-driven
  // catalogs). Overwrites any existing entry for that id. Returns `id`.
  ItemDefId Register(ItemDefId id, const ItemDef& def);
  // Definition for `id`, or nullptr if unknown (id 0 is always unknown). The
  // pointer stays valid until that entry is removed or the catalog dies.
  const ItemDef* Find(ItemDefId id) const;
  bool Contains(ItemDefId id) const { return Find(id) != nullptr; }
  void Remove(ItemDefId id);
  void Clear();
  size_t size() const { return defs_.size(); }

 private:
  std::unordered_map<ItemDefId, ItemDef> defs_;
  ItemDefId next_id_ = 1;
};

}  // namespace rx::inventory

#endif  // RX_INVENTORY_ITEM_CATALOG_H_
