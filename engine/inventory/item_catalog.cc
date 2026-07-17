#include "inventory/item_catalog.h"

namespace rx::inventory {

ItemDefId ItemCatalog::Register(const ItemDef& def) {
  while (next_id_ == kInvalidItemDef || defs_.count(next_id_) != 0) ++next_id_;
  ItemDefId id = next_id_++;
  defs_[id] = def;
  return id;
}

ItemDefId ItemCatalog::Register(ItemDefId id, const ItemDef& def) {
  defs_[id] = def;
  if (id != kInvalidItemDef && id >= next_id_) next_id_ = id + 1;
  return id;
}

const ItemDef* ItemCatalog::Find(ItemDefId id) const {
  if (id == kInvalidItemDef) return nullptr;
  auto it = defs_.find(id);
  return it == defs_.end() ? nullptr : &it->second;
}

void ItemCatalog::Remove(ItemDefId id) { defs_.erase(id); }

void ItemCatalog::Clear() {
  defs_.clear();
  next_id_ = 1;
}

}  // namespace rx::inventory
