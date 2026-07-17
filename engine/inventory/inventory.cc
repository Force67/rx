#include "inventory/inventory.h"

namespace rx::inventory {
namespace {

u32 EffectiveMaxStack(const ItemDef& def) { return def.max_stack == 0 ? 1u : def.max_stack; }

u32 NonEmptyStacks(const Inventory& inv) {
  u32 c = 0;
  for (const auto& e : inv.entries)
    if (e.count > 0) ++c;
  return c;
}

bool Holds(const Inventory& inv, ItemDefId item, u64 payload) {
  for (const auto& e : inv.entries)
    if (e.item == item && e.payload == payload && e.count > 0) return true;
  return false;
}

}  // namespace

u32 InventoryCount(const Inventory& inv, ItemDefId item) {
  u32 total = 0;
  for (const auto& e : inv.entries)
    if (e.item == item) total += e.count;
  return total;
}

f32 InventoryWeight(const Inventory& inv, const ItemCatalog& catalog) {
  f32 weight = 0;
  for (const auto& e : inv.entries) {
    if (e.count == 0) continue;
    const ItemDef* def = catalog.Find(e.item);
    if (def) weight += def->weight * f32(e.count);
  }
  return weight;
}

u32 AddItem(Inventory& inv, const ItemCatalog& catalog, ItemDefId item, u32 count, u64 payload) {
  if (count == 0) return 0;
  const ItemDef* def = catalog.Find(item);
  if (!def) return 0;

  u32 remaining = count;

  // Weight cap: clamp up front so we never overshoot max_weight.
  if (def->weight > 0 && inv.max_weight > 0) {
    f32 free = inv.max_weight - InventoryWeight(inv, catalog);
    if (free <= 0) return 0;
    u32 by_weight = u32(free / def->weight);
    if (by_weight < remaining) remaining = by_weight;
    if (remaining == 0) return 0;
  }

  const u32 max_stack = EffectiveMaxStack(*def);
  u32 added = 0;

  // 1) Top up existing stacks of the same (item, payload).
  for (auto& e : inv.entries) {
    if (remaining == 0) break;
    if (e.count == 0 || e.item != item || e.payload != payload) continue;
    u32 space = max_stack > e.count ? max_stack - e.count : 0;
    u32 n = space < remaining ? space : remaining;
    e.count += n;
    remaining -= n;
    added += n;
  }

  // 2) Reuse empty slots / append new stacks, bounded by max_entries.
  while (remaining > 0) {
    if (inv.max_entries != 0 && NonEmptyStacks(inv) >= inv.max_entries) break;
    u32 n = max_stack < remaining ? max_stack : remaining;
    InventoryEntry* slot = nullptr;
    for (auto& e : inv.entries) {
      if (e.count == 0) {
        slot = &e;
        break;
      }
    }
    if (slot) {
      slot->item = item;
      slot->payload = payload;
      slot->count = n;
    } else {
      inv.entries.push_back({item, n, payload});
    }
    remaining -= n;
    added += n;
  }

  if (added > 0) ++inv.revision;
  return added;
}

u32 RemoveItem(Inventory& inv, ItemDefId item, u32 count, u64 payload) {
  if (count == 0) return 0;
  u32 removed = 0;
  for (auto& e : inv.entries) {
    if (removed >= count) break;
    if (e.count == 0 || e.item != item || e.payload != payload) continue;
    u32 n = e.count < (count - removed) ? e.count : (count - removed);
    e.count -= n;
    removed += n;
    if (e.count == 0) {
      e.item = kInvalidItemDef;
      e.payload = 0;
    }
  }
  if (removed > 0) ++inv.revision;
  return removed;
}

u32 TransferItem(Inventory& src, Inventory& dst, const ItemCatalog& catalog, ItemDefId item,
                 u32 count, u64 payload) {
  if (count == 0) return 0;
  u32 available = 0;
  for (const auto& e : src.entries)
    if (e.item == item && e.payload == payload) available += e.count;
  u32 want = count < available ? count : available;
  if (want == 0) return 0;
  // Add to dst first (it may accept fewer than `want` on capacity), then remove
  // exactly what landed from src so nothing is ever duplicated or lost.
  u32 moved = AddItem(dst, catalog, item, want, payload);
  if (moved > 0) RemoveItem(src, item, moved, payload);
  return moved;
}

EquipmentSlot* FindSlot(Equipment& eq, u32 tag) {
  for (auto& s : eq.slots)
    if (s.tag == tag) return &s;
  return nullptr;
}

bool Equip(const Inventory& inv, Equipment& eq, u32 tag, ItemDefId item, u64 payload) {
  if (!Holds(inv, item, payload)) return false;
  EquipmentSlot* slot = FindSlot(eq, tag);
  if (!slot) {
    eq.slots.push_back(EquipmentSlot{tag, kInvalidItemDef, 0, false});
    slot = &eq.slots.back();
  }
  slot->item = item;
  slot->payload = payload;
  slot->occupied = true;
  ++eq.revision;
  return true;
}

bool Unequip(Equipment& eq, u32 tag) {
  EquipmentSlot* slot = FindSlot(eq, tag);
  if (!slot || !slot->occupied) return false;
  slot->item = kInvalidItemDef;
  slot->payload = 0;
  slot->occupied = false;
  ++eq.revision;
  return true;
}

u32 ValidateEquipment(const Inventory& inv, Equipment& eq) {
  u32 cleared = 0;
  for (auto& s : eq.slots) {
    if (!s.occupied) continue;
    if (Holds(inv, s.item, s.payload)) continue;
    s.item = kInvalidItemDef;
    s.payload = 0;
    s.occupied = false;
    ++cleared;
  }
  if (cleared > 0) ++eq.revision;
  return cleared;
}

}  // namespace rx::inventory
