#ifndef RX_ECS_WORLD_H_
#define RX_ECS_WORLD_H_

#include <tuple>
#include <utility>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "ecs/archetype.h"
#include "ecs/component.h"
#include "ecs/entity.h"

namespace rx::ecs {

class RX_ECS_EXPORT World {
 public:
  World();
  ~World();

  Entity Create();
  void Destroy(Entity entity);
  bool IsAlive(Entity entity) const;

  template <typename T>
  T& Add(Entity entity, T value) {
    void* slot = AddRaw(entity, GetComponentId<T>());
    return *new (slot) T(std::move(value));
  }

  template <typename T>
  void Remove(Entity entity) {
    RemoveRaw(entity, GetComponentId<T>());
  }

  template <typename T>
  T* Get(Entity entity) {
    return static_cast<T*>(GetRaw(entity, GetComponentId<T>()));
  }

  template <typename T>
  bool Has(Entity entity) const {
    return HasRaw(entity, GetComponentId<T>());
  }

  // Calls fn(Entity, Ts&...) for every entity that has all of Ts.
  template <typename... Ts, typename Fn>
  void Each(Fn&& fn) {
    Signature required = MakeSignature({GetComponentId<Ts>()...});
    for (auto& archetype : archetypes_) {
      if (!SignatureContainsAll(archetype->signature(), required)) continue;
      auto columns = std::make_tuple(static_cast<Ts*>(archetype->ColumnData(GetComponentId<Ts>()))...);
      for (u32 row = 0; row < archetype->row_count(); ++row) {
        fn(archetype->entity_at(row), std::get<Ts*>(columns)[row]...);
      }
    }
  }

  size_t entity_count() const { return live_count_; }

  struct Stats {
    u32 entity_count = 0;
    u32 entity_slots = 0;
    u32 archetype_count = 0;
    u64 live_component_bytes = 0;
    u64 component_capacity_bytes = 0;
  };
  Stats stats() const;

  // Untyped access, used by replication and converters.
  void* AddRaw(Entity entity, ComponentId id);
  void RemoveRaw(Entity entity, ComponentId id);
  void* GetRaw(Entity entity, ComponentId id);
  bool HasRaw(Entity entity, ComponentId id) const;

 private:
  struct EntityRecord {
    Archetype* archetype = nullptr;
    u32 row = 0;
    u32 generation = 0;
    bool alive = false;
  };

  Archetype* GetOrCreateArchetype(const Signature& signature);
  void MoveEntity(Entity entity, EntityRecord& record, Archetype* destination);

  base::Vector<EntityRecord> records_;
  base::Vector<u32> free_indices_;
  base::Vector<base::UniquePointer<Archetype>> archetypes_;
  base::UnorderedMap<Signature, Archetype*, SignatureHash, SignatureEqual> archetype_lookup_;
  size_t live_count_ = 0;
};

}  // namespace rx::ecs

#endif  // RX_ECS_WORLD_H_
