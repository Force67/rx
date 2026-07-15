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
  // Allocation-free: the required set lives on the stack and iteration walks
  // each archetype chunk by chunk. Structural changes from fn are memory-safe,
  // but can skip or revisit entities; defer them when snapshot semantics matter.
  template <typename... Ts, typename Fn>
  void Each(Fn&& fn) {
    static_assert(sizeof...(Ts) > 0);
    ComponentId required[sizeof...(Ts)] = {GetComponentId<Ts>()...};
    std::sort(std::begin(required), std::end(required));
    // Archetypes themselves are stable, but structural changes in a callback
    // may reallocate archetypes_ or shrink the current chunk. Iterate the
    // original archetype set by pointer and revalidate the row each time.
    const size_t archetype_count = archetypes_.size();
    for (size_t archetype_index = 0; archetype_index < archetype_count; ++archetype_index) {
      Archetype* archetype =
          archetypes_[archetype_index].Get_UseOnlyIfYouKnowWhatYouareDoing();
      if (!SignatureContainsAll(archetype->signature(), required, sizeof...(Ts))) continue;
      const u32 rows_per_chunk = archetype->rows_per_chunk();
      const u32 initial_row_count = archetype->row_count();
      int indices[sizeof...(Ts)] = {archetype->ColumnIndex(GetComponentId<Ts>())...};
      for (u32 row = 0;
           row < initial_row_count && row < archetype->row_count(); ++row) {
        const u32 chunk = row / rows_per_chunk;
        const u32 in_chunk = row % rows_per_chunk;
        std::tuple<Ts*...> columns = [&]<size_t... Is>(std::index_sequence<Is...>) {
          return std::tuple<Ts*...>{
              static_cast<Ts*>(archetype->ChunkColumnData(chunk, indices[Is]))...};
        }(std::index_sequence_for<Ts...>{});
        fn(archetype->entity_at(row), std::get<Ts*>(columns)[in_chunk]...);
      }
    }
  }

  size_t entity_count() const { return live_count_; }

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
