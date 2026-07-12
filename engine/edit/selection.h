#ifndef RX_EDIT_SELECTION_H_
#define RX_EDIT_SELECTION_H_

#include <span>
#include <vector>

#include "core/export.h"
#include "ecs/entity.h"

namespace rx::edit {

// The set of currently selected entities. The "primary" is the anchor for
// single-target operations (the gizmo pivot, the inspector's subject): the most
// recently added or set entity, and it stays valid as long as it is in the set.
class RX_EDIT_EXPORT Selection {
 public:
  void Clear();
  // Replaces the selection with a single entity (becomes primary).
  void Set(ecs::Entity entity);
  // Adds an entity (becomes primary); a no-op if already present except that it
  // still becomes primary.
  void Add(ecs::Entity entity);
  // Adds if absent (becomes primary), removes if present.
  void Toggle(ecs::Entity entity);
  bool Contains(ecs::Entity entity) const;

  ecs::Entity primary() const { return primary_; }
  std::span<const ecs::Entity> entities() const { return entities_; }
  bool empty() const { return entities_.empty(); }
  size_t size() const { return entities_.size(); }

 private:
  std::vector<ecs::Entity> entities_;
  ecs::Entity primary_{};
};

}  // namespace rx::edit

#endif  // RX_EDIT_SELECTION_H_
