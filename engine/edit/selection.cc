#include "edit/selection.h"

#include <algorithm>

namespace rx::edit {

void Selection::Clear() {
  entities_.clear();
  primary_ = ecs::kInvalidEntity;
}

void Selection::Set(ecs::Entity entity) {
  entities_.clear();
  if (entity) {
    entities_.push_back(entity);
    primary_ = entity;
  } else {
    primary_ = ecs::kInvalidEntity;
  }
}

void Selection::Add(ecs::Entity entity) {
  if (!entity) return;
  if (!Contains(entity)) entities_.push_back(entity);
  primary_ = entity;
}

void Selection::Toggle(ecs::Entity entity) {
  if (!entity) return;
  auto it = std::find(entities_.begin(), entities_.end(), entity);
  if (it != entities_.end()) {
    entities_.erase(it);
    if (primary_ == entity) primary_ = entities_.empty() ? ecs::kInvalidEntity : entities_.back();
  } else {
    entities_.push_back(entity);
    primary_ = entity;
  }
}

bool Selection::Contains(ecs::Entity entity) const {
  return std::find(entities_.begin(), entities_.end(), entity) != entities_.end();
}

}  // namespace rx::edit
