#ifndef RX_EDIT_UNDO_H_
#define RX_EDIT_UNDO_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/export.h"
#include "ecs/entity.h"
#include "ecs/world.h"
#include "edit/reflect.h"

// Undo/redo built on reversible Commands. Commands identify their target
// entities by Guid, not by ecs handle, so a target destroyed and recreated by
// other undo/redo steps is re-resolved through the world rather than dangling
// (see edit::FindByGuid). The factory helpers snapshot whatever prior state
// their Revert needs at construction time.
namespace rx::edit {

class Command {
public:
  virtual ~Command() = default;
  virtual void Apply(ecs::World &world) = 0;
  virtual void Revert(ecs::World &world) = 0;
  virtual const char *label() const = 0;
};

class RX_EDIT_EXPORT UndoStack {
public:
  // Applies `cmd` immediately, then records it (clearing the redo stack).
  // Inside a group the command is applied and buffered into the group.
  void Push(ecs::World &world, std::unique_ptr<Command> cmd);

  // Records an interaction that already updated its target live (for example a
  // terrain brush stroke). Undo calls Revert; redo calls Apply as usual.
  void RecordApplied(std::unique_ptr<Command> cmd);

  bool Undo(ecs::World &world);
  bool Redo(ecs::World &world);

  // Coalesces every command pushed until the matching EndGroup into one undo
  // step labelled `label`. Groups may nest; only the outermost boundary
  // records.
  void BeginGroup(const char *label);
  void EndGroup();

  bool can_undo() const { return !undo_.empty(); }
  bool can_redo() const { return !redo_.empty(); }
  const char *undo_label() const {
    return undo_.empty() ? "" : undo_.back()->label();
  }
  const char *redo_label() const {
    return redo_.empty() ? "" : redo_.back()->label();
  }

  void Clear();
  size_t size() const { return undo_.size(); }

private:
  std::vector<std::unique_ptr<Command>> undo_;
  std::vector<std::unique_ptr<Command>> redo_;
  std::vector<std::unique_ptr<Command>> group_buffer_;
  std::string group_label_;
  int group_depth_ = 0;
};

// Sets a single reflected field. Captures the current value for Revert.
RX_EDIT_EXPORT std::unique_ptr<Command>
MakeSetProp(ecs::World &world, ecs::Entity entity, const ComponentDesc &comp,
            const PropDesc &prop, PropValue new_value);

// Creates an entity with the given components and initial prop values. The
// entity's Guid is stable across undo/redo; *out_entity receives the live
// handle on each Apply (may be null).
RX_EDIT_EXPORT std::unique_ptr<Command> MakeCreateEntity(
    std::vector<std::pair<const ComponentDesc *,
                          std::vector<std::pair<const PropDesc *, PropValue>>>>
        initial,
    ecs::Entity *out_entity);

// Destroys an entity; snapshots all reflected components and their props (and
// its Guid) so Revert recreates it faithfully.
RX_EDIT_EXPORT std::unique_ptr<Command> MakeDestroyEntity(ecs::World &world,
                                                          ecs::Entity entity);

// Reparents an entity, preserving its world transform. An invalid new_parent
// unparents it.
RX_EDIT_EXPORT std::unique_ptr<Command>
MakeReparent(ecs::World &world, ecs::Entity entity, ecs::Entity new_parent);

// Adds a default-constructed component (no-op if already present). Revert
// removes it (only if this command added it).
RX_EDIT_EXPORT std::unique_ptr<Command>
MakeAddComponent(ecs::World &world, ecs::Entity entity,
                 const ComponentDesc &comp);

// Removes a component; snapshots its props so Revert restores them.
RX_EDIT_EXPORT std::unique_ptr<Command>
MakeRemoveComponent(ecs::World &world, ecs::Entity entity,
                    const ComponentDesc &comp);

} // namespace rx::edit

#endif // RX_EDIT_UNDO_H_
