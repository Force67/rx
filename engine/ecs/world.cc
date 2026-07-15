#include "ecs/world.h"

#include <cassert>
#include <mutex>

#include <base/check.h>

namespace rx::ecs {

namespace detail {
namespace {

// Every DSO has its own ComponentIdFor<T> cache, so first use resolves a stable
// compile-time type key through this one process-wide registry. Registration is
// cold and locked; reads by already-resolved IDs remain lock-free.
struct Registry {
  // Generous ceiling; the engine + a large game register on the order of a few
  // hundred component types. Overflow is a hard error, not silent corruption.
  static constexpr u32 kMaxComponents = 4096;
  std::mutex mutex;
  u32 count = 0;
  u64 keys[kMaxComponents]{};
  ComponentInfo infos[kMaxComponents]{};
};

Registry& TheRegistry() {
  static Registry registry;
  return registry;
}

}  // namespace

ComponentId ResolveComponentId(u64 type_key, const ComponentInfo& info) {
  Registry& registry = TheRegistry();
  std::lock_guard lock(registry.mutex);
  for (u32 id = 0; id < registry.count; ++id) {
    if (registry.keys[id] != type_key) continue;
    BASE_BUGCHECK(registry.infos[id].size == info.size && registry.infos[id].align == info.align,
                  "component type-key collision");
    return id;
  }

  BASE_BUGCHECK(registry.count < Registry::kMaxComponents, "component-type registry overflow");
  const ComponentId id = registry.count++;
  registry.keys[id] = type_key;
  registry.infos[id] = info;
  return id;
}

}  // namespace detail

const ComponentInfo& GetComponentInfo(ComponentId id) {
  detail::Registry& registry = detail::TheRegistry();
#ifndef NDEBUG
  std::lock_guard lock(registry.mutex);
  assert(id < registry.count && "unregistered component id");
#endif
  return registry.infos[id];
}

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

World::Stats World::stats() const {
  Stats result;
  result.entity_count = static_cast<u32>(live_count_);
  result.entity_slots = static_cast<u32>(records_.size());
  result.archetype_count = static_cast<u32>(archetypes_.size());
  for (const auto& archetype : archetypes_) {
    const Archetype::StorageStats storage = archetype->storage_stats();
    result.live_component_bytes += storage.live_bytes;
    result.component_capacity_bytes += storage.capacity_bytes;
  }
  return result;
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
