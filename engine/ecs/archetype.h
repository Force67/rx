#ifndef RX_ECS_ARCHETYPE_H_
#define RX_ECS_ARCHETYPE_H_

#include <algorithm>
#include <cstring>

#include <base/containers/vector.h>
#include <base/hashing/fnv1a.h>

#include "ecs/component.h"
#include "ecs/entity.h"

namespace rx::ecs {

// Sorted set of component ids identifying an archetype.
using Signature = base::Vector<ComponentId>;

inline Signature MakeSignature(std::initializer_list<ComponentId> ids) {
  Signature sig(ids);
  std::sort(sig.begin(), sig.end());
  return sig;
}

inline bool SignatureContains(const Signature& sig, ComponentId id) {
  return std::binary_search(sig.begin(), sig.end(), id);
}

inline bool SignatureContainsAll(const Signature& sig, const Signature& subset) {
  return std::includes(sig.begin(), sig.end(), subset.begin(), subset.end());
}

// Functors for hashing signatures as raw bytes (sorted ids make this stable).
struct SignatureHash {
  mem_size operator()(const Signature& sig) const {
    return base::fnv1a(reinterpret_cast<const u8*>(sig.data()),
                       sig.size() * sizeof(ComponentId));
  }
};

struct SignatureEqual {
  bool operator()(const Signature& a, const Signature& b) const {
    return a.size() == b.size() &&
           (a.empty() ||
            std::memcmp(a.data(), b.data(), a.size() * sizeof(ComponentId)) == 0);
  }
};

// Columnar storage. One column per component type, rows are entities.
class RX_ECS_EXPORT Archetype {
 public:
  explicit Archetype(Signature signature);
  ~Archetype();

  Archetype(const Archetype&) = delete;
  Archetype& operator=(const Archetype&) = delete;

  // Appends a row with uninitialized component memory, returns the row index.
  u32 AddRow(Entity entity);

  // Swap removes a row, destructing its components. Returns the entity that
  // was moved into the vacated row, or kInvalidEntity if the last row was removed.
  Entity SwapRemoveRow(u32 row);

  void* ComponentAt(ComponentId id, u32 row);
  void* ColumnData(ComponentId id);
  int ColumnIndex(ComponentId id) const;

  const Signature& signature() const { return signature_; }
  u32 row_count() const { return static_cast<u32>(entities_.size()); }
  Entity entity_at(u32 row) const { return entities_[row]; }

 private:
  struct Column {
    Column(ComponentId component_id, u32 component_stride, u32 component_align)
        : id(component_id), stride(component_stride), align(component_align) {}
    ~Column();

    Column(Column&& other) noexcept;
    Column& operator=(Column&& other) noexcept;

    Column(const Column&) = delete;
    Column& operator=(const Column&) = delete;

    void Reserve(u32 row_capacity, u32 live_rows);

    ComponentId id;
    u32 stride;
    u32 align;
    u8* data = nullptr;
    u32 capacity = 0;
  };

  Signature signature_;
  base::Vector<Column> columns_;
  base::Vector<Entity> entities_;
};

}  // namespace rx::ecs

#endif  // RX_ECS_ARCHETYPE_H_
