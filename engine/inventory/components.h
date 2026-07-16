#ifndef RX_INVENTORY_COMPONENTS_H_
#define RX_INVENTORY_COMPONENTS_H_

#include <vector>

#include "core/types.h"
#include "inventory/item_catalog.h"

namespace rx::inventory {

// One stack in an inventory. Two entries with the same item but different
// `payload` never merge: payload identifies a unique instance (durability,
// enchantment handle, serial number - whatever the game stores). A `count` of 0
// marks an empty slot that AddItem may reuse.
struct InventoryEntry {
  ItemDefId item = kInvalidItemDef;
  u32 count = 0;
  u64 payload = 0;
};

// Attach to any entity that can hold items (players, NPCs, chests, corpses).
// Plain data: every mutation goes through the free functions in inventory.h so
// capacity and the revision counter stay consistent. Non-POD (owns a vector);
// the ECS relocates it through its move constructor, which is supported.
struct Inventory {
  std::vector<InventoryEntry> entries;
  f32 max_weight = 0;   // 0 == unlimited
  u32 max_entries = 0;  // 0 == unlimited (max distinct non-empty stacks)
  u32 revision = 0;     // bumped on every change; UIs diff against it
};

// One equipment slot. `tag` is a game-hashed semantic ("hand.right", "head",
// "ring.1"); the engine never interprets it. A slot references an item that
// remains owned by the Inventory (the source of truth) by (item, payload); it
// stores no count of its own.
struct EquipmentSlot {
  u32 tag = 0;
  ItemDefId item = kInvalidItemDef;
  u64 payload = 0;
  bool occupied = false;
};

// Attach beside an Inventory to give an entity equipment slots. Slots are
// created on demand by Equip; games may also pre-populate `slots` with the tags
// they support.
struct Equipment {
  std::vector<EquipmentSlot> slots;
  u32 revision = 0;
};

// A dropped item living in the 3D world as a physics body. Created by DropItem,
// consumed by PickUpItem, kept alive cheaply between (SyncWorldItems) and
// removed from the physics world by hibernation. `body` is a physics::BodyId
// stored as a raw u64 so this header stays physics-free; it is 0 when the item
// has no live body (mid-hibernation transient).
struct WorldItem {
  ItemDefId item = kInvalidItemDef;
  u32 count = 1;
  u64 payload = 0;
  u64 body = 0;           // physics::BodyId; 0 => no live body
  u64 persistent_id = 0;  // stable across save/load and hibernate/wake
  f32 last_pos[3] = {0, 0, 0};
  u16 still_frames = 0;   // consecutive near-motionless syncs
  bool at_rest = false;   // settled: SyncWorldItems stops polling it
};

}  // namespace rx::inventory

#endif  // RX_INVENTORY_COMPONENTS_H_
