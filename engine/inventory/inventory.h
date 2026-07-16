#ifndef RX_INVENTORY_INVENTORY_H_
#define RX_INVENTORY_INVENTORY_H_

#include "inventory/components.h"
#include "inventory/export.h"
#include "inventory/item_catalog.h"

// Free-function operations over Inventory/Equipment components (functional-first
// house style, like scene/camera_rig.h). No world or physics dependency: this
// header is enough for pure inventory logic (containers, crafting, trade UIs).

namespace rx::inventory {

// --- queries ---

// Total units of `item` held (summed over every payload variant).
RX_INVENTORY_EXPORT u32 InventoryCount(const Inventory& inv, ItemDefId item);
// Summed weight = sum(count * ItemDef::weight). Unknown defs contribute 0.
RX_INVENTORY_EXPORT f32 InventoryWeight(const Inventory& inv, const ItemCatalog& catalog);

// --- mutation ---

// Adds up to `count` units of (item, payload), first topping up existing stacks
// (bounded by ItemDef::max_stack), then filling empty slots / appending new
// stacks (bounded by Inventory::max_entries and max_weight). Returns the number
// actually added: 0 if the def is unknown or nothing fits. Bumps revision when
// the result is > 0.
RX_INVENTORY_EXPORT u32 AddItem(Inventory& inv, const ItemCatalog& catalog, ItemDefId item,
                                u32 count, u64 payload = 0);
// Removes up to `count` units of (item, payload) - an EXACT payload match, so
// pass the same payload the units were added with. Returns the number removed;
// bumps revision when > 0. Empty entries are left in place for reuse.
RX_INVENTORY_EXPORT u32 RemoveItem(Inventory& inv, ItemDefId item, u32 count, u64 payload = 0);
// Moves up to `count` of (item, payload) from `src` into `dst`, honouring dst's
// capacity/weight (used for looting, containers). Returns the number moved; src
// loses exactly that many.
RX_INVENTORY_EXPORT u32 TransferItem(Inventory& src, Inventory& dst, const ItemCatalog& catalog,
                                     ItemDefId item, u32 count, u64 payload = 0);

// --- equipment ---

RX_INVENTORY_EXPORT EquipmentSlot* FindSlot(Equipment& eq, u32 tag);
// Marks (item, payload) equipped in slot `tag` (created if absent) provided the
// inventory holds at least one. Inventory keeps ownership: nothing is moved or
// removed, so counts stay the source of truth. Returns false if the inventory
// lacks the item.
RX_INVENTORY_EXPORT bool Equip(const Inventory& inv, Equipment& eq, u32 tag, ItemDefId item,
                               u64 payload = 0);
// Clears slot `tag`. Returns true if it was occupied.
RX_INVENTORY_EXPORT bool Unequip(Equipment& eq, u32 tag);
// Unequips any slot whose referenced (item, payload) is no longer present in
// the inventory (call after RemoveItem/TransferItem/DropItem). Returns the
// number of slots cleared.
RX_INVENTORY_EXPORT u32 ValidateEquipment(const Inventory& inv, Equipment& eq);

}  // namespace rx::inventory

#endif  // RX_INVENTORY_INVENTORY_H_
