#include "plugins/inventory/item_catalog.h"

namespace rx::inventory {

namespace {
// Overwrites in place, so a pointer from an earlier Find stays valid.
void Store(base::UnorderedMap<ItemDefId, base::UniquePointer<ItemDef>>& defs, ItemDefId id,
           const ItemDef& def) {
  base::UniquePointer<ItemDef>& slot = defs[id];
  if (slot) {
    *slot = def;
  } else {
    slot = base::MakeUnique<ItemDef>(def);
  }
}
}  // namespace

ItemDefId ItemCatalog::Register(const ItemDef& def) {
  while (next_id_ == kInvalidItemDef || defs_.count(next_id_) != 0) ++next_id_;
  ItemDefId id = next_id_++;
  Store(defs_, id, def);
  return id;
}

ItemDefId ItemCatalog::Register(ItemDefId id, const ItemDef& def) {
  Store(defs_, id, def);
  if (id != kInvalidItemDef && id >= next_id_) next_id_ = id + 1;
  return id;
}

const ItemDef* ItemCatalog::Find(ItemDefId id) const {
  if (id == kInvalidItemDef) return nullptr;
  const base::UniquePointer<ItemDef>* found = defs_.find(id);
  return found ? &**found : nullptr;
}

void ItemCatalog::Remove(ItemDefId id) { defs_.erase(id); }

void ItemCatalog::Clear() {
  defs_.clear();
  next_id_ = 1;
}

}  // namespace rx::inventory
