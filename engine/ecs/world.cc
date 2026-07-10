#include "ecs/world.h"

#include <atomic>
#include <cassert>

namespace rx::ecs {

namespace detail {
namespace {

// The one and only component-type registry. It is a function-local static
// living in this translation unit (the ecs DSO); NextComponentId /
// RegisterComponent / GetComponentInfo are the only ways to touch it and they
// are exported, so under RX_SHARED there is exactly one instance process-wide
// even though the template that drives registration (ComponentIdFor<T>) is
// instantiated in every consumer DSO.
//
// Thread-safety without a lock on the hot read path:
//   - `next` is an atomic counter (fetch_add), so id assignment is safe.
//   - `infos` is a FIXED-capacity array: it never reallocates, so a write to
//     one slot never races a read of another (unlike the old resizing Vector).
//   - Each type registers exactly once, from the initializer of its
//     ComponentIdFor<T> function-local static. That per-type guard has
//     acquire/release semantics, and (because ComponentIdFor<T> is forced to
//     default visibility) it is a single guard process-wide. Any code that
//     holds an id obtained it by returning through that guard, which
//     happens-before publishes the matching infos[id] write. So GetComponentInfo
//     is a plain array read: no atomics, no lock.
struct Registry {
  // Generous ceiling; the engine + a large game register on the order of a few
  // hundred component types. Overflow is a hard error, not silent corruption.
  static constexpr u32 kMaxComponents = 4096;
  std::atomic<ComponentId> next{0};
  ComponentInfo infos[kMaxComponents]{};
};

Registry& TheRegistry() {
  static Registry registry;
  return registry;
}

}  // namespace

ComponentId NextComponentId() {
  const ComponentId id = TheRegistry().next.fetch_add(1, std::memory_order_relaxed);
  assert(id < Registry::kMaxComponents && "component-type registry overflow");
  return id;
}

void RegisterComponent(ComponentId id, const ComponentInfo& info) {
  if (id >= Registry::kMaxComponents) return;  // overflow already asserted above
  TheRegistry().infos[id] = info;
}

}  // namespace detail

const ComponentInfo& GetComponentInfo(ComponentId id) { return detail::TheRegistry().infos[id]; }

World::World() { GetOrCreateArchetype({}); }

World::~World() = default;

Entity World::Create() {
  u32 index;
  if (!free_indices_.empty()) {
    index = free_indices_.back();
    free_indices_.pop_back();
  } else {
    index = static_cast<u32>(records_.size());
    records_.emplace_back();
  }
  EntityRecord& record = records_[index];
  record.alive = true;
  Entity entity{index, record.generation};
  record.archetype = archetypes_.front().Get_UseOnlyIfYouKnowWhatYouareDoing();
  record.row = record.archetype->AddRow(entity);
  ++live_count_;
  return entity;
}

void World::Destroy(Entity entity) {
  if (!IsAlive(entity)) return;
  EntityRecord& record = records_[entity.index];
  Entity moved = record.archetype->SwapRemoveRow(record.row);
  if (moved) records_[moved.index].row = record.row;
  record.alive = false;
  record.archetype = nullptr;
  ++record.generation;
  free_indices_.push_back(entity.index);
  --live_count_;
}

bool World::IsAlive(Entity entity) const {
  return entity.index < records_.size() && records_[entity.index].alive &&
         records_[entity.index].generation == entity.generation;
}

void* World::AddRaw(Entity entity, ComponentId id) {
  EntityRecord& record = records_[entity.index];
  if (void* existing = record.archetype->ComponentAt(id, record.row)) {
    GetComponentInfo(id).destruct(existing);
    return existing;
  }
  Signature signature = record.archetype->signature();
  signature.insert(std::lower_bound(signature.begin(), signature.end(), id), id);
  MoveEntity(entity, record, GetOrCreateArchetype(signature));
  return record.archetype->ComponentAt(id, record.row);
}

void World::RemoveRaw(Entity entity, ComponentId id) {
  EntityRecord& record = records_[entity.index];
  if (!SignatureContains(record.archetype->signature(), id)) return;
  Signature signature = record.archetype->signature();
  signature.erase(std::lower_bound(signature.begin(), signature.end(), id));
  MoveEntity(entity, record, GetOrCreateArchetype(signature));
}

void* World::GetRaw(Entity entity, ComponentId id) {
  if (!IsAlive(entity)) return nullptr;
  EntityRecord& record = records_[entity.index];
  return record.archetype->ComponentAt(id, record.row);
}

bool World::HasRaw(Entity entity, ComponentId id) const {
  if (!IsAlive(entity)) return false;
  const EntityRecord& record = records_[entity.index];
  return SignatureContains(record.archetype->signature(), id);
}

Archetype* World::GetOrCreateArchetype(const Signature& signature) {
  if (Archetype** found = archetype_lookup_.find(signature)) return *found;
  archetypes_.push_back(base::MakeUnique<Archetype>(signature));
  Archetype* archetype = archetypes_.back().Get_UseOnlyIfYouKnowWhatYouareDoing();
  archetype_lookup_.insert(signature, archetype);
  return archetype;
}

void World::MoveEntity(Entity entity, EntityRecord& record, Archetype* destination) {
  Archetype* source = record.archetype;
  u32 source_row = record.row;
  u32 destination_row = destination->AddRow(entity);
  for (ComponentId id : destination->signature()) {
    void* from = source->ComponentAt(id, source_row);
    if (!from) continue;
    GetComponentInfo(id).move_construct(destination->ComponentAt(id, destination_row), from);
  }
  Entity moved = source->SwapRemoveRow(source_row);
  if (moved) records_[moved.index].row = source_row;
  record.archetype = destination;
  record.row = destination_row;
}

}  // namespace rx::ecs
