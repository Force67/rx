#include "combat/weapon_def.h"

#include <algorithm>

namespace rx::combat {

WeaponDefId WeaponCatalog::Register(const WeaponDef& def) {
  return Register(next_id_, def);
}

WeaponDefId WeaponCatalog::Register(WeaponDefId id, const WeaponDef& def) {
  if (id == kInvalidWeaponDef) return kInvalidWeaponDef;
  defs_[id] = def;
  if (id >= next_id_) next_id_ = id + 1;
  return id;
}

const WeaponDef* WeaponCatalog::Find(WeaponDefId id) const {
  if (id == kInvalidWeaponDef) return nullptr;
  return defs_.find(id);
}

void WeaponCatalog::Remove(WeaponDefId id) { defs_.erase(id); }

void WeaponCatalog::Clear() {
  defs_.clear();
  next_id_ = 1;
}

f32 FalloffScale(const WeaponDef& def, f32 distance) {
  if (def.falloff_end <= def.falloff_start) return 1.0f;
  if (distance <= def.falloff_start) return 1.0f;
  const f32 min_scale = std::clamp(def.falloff_min_scale, 0.0f, 1.0f);
  if (distance >= def.falloff_end) return min_scale;
  const f32 t = (distance - def.falloff_start) / (def.falloff_end - def.falloff_start);
  return 1.0f + (min_scale - 1.0f) * t;
}

f32 ShotInterval(const WeaponDef& def) {
  if (def.rpm <= 0) return 0;
  return 60.0f / def.rpm;
}

}  // namespace rx::combat
