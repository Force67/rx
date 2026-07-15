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

// Allocation-free variant for queries (`ids` must be sorted).
inline bool SignatureContainsAll(const Signature& sig, const ComponentId* ids, size_t count) {
  return std::includes(sig.begin(), sig.end(), ids, ids + count);
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
//
// Rows live in fixed-size chunks drawn from mem::GlobalChunkPool (16 KiB),
// laid out SoA within each chunk: column c of chunk k is the array at
// chunk[k] + column_offset[c], rows_per_chunk elements long. Growing an
// archetype appends a chunk — existing rows never relocate, so components are
// only ever moved through their typed move_construct (swap-remove and
// archetype transitions), never memcpy'd. Emptied tail chunks return to the
// pool. Archetypes whose row (or component alignment) exceeds the pool chunk
// fall back to dedicated heap chunks of one row each.
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
  int ColumnIndex(ComponentId id) const;

  // Chunked iteration (World::Each): base pointer of one column's array
  // within one chunk. Rows [chunk * rows_per_chunk, ...) map to elements
  // [0, ChunkRowCount(chunk)) of that array.
  void* ChunkColumnData(u32 chunk, int column_index);
  u32 chunk_count() const { return static_cast<u32>(chunks_.size()); }
  u32 rows_per_chunk() const { return rows_per_chunk_; }
  u32 ChunkRowCount(u32 chunk) const;

  const Signature& signature() const { return signature_; }
  u32 row_count() const { return static_cast<u32>(entities_.size()); }
  Entity entity_at(u32 row) const { return entities_[row]; }

  private:
  struct Column {
    ComponentId id;
    u32 stride;
    u32 chunk_offset;  // byte offset of this column's array within a chunk
  };

  void* AcquireChunk();
  void ReleaseChunk(void* chunk);
  u8* RowAddress(const Column& column, u32 row);

  Signature signature_;
  base::Vector<Column> columns_;
  base::Vector<void*> chunks_;
  base::Vector<Entity> entities_;
  u32 rows_per_chunk_ = 0;
  u32 chunk_bytes_ = 0;   // == pool chunk size unless oversized
  u32 chunk_align_ = 0;
  bool pooled_ = true;    // false: dedicated heap chunks (oversized row/align)
};

}  // namespace rx::ecs

#endif  // RX_ECS_ARCHETYPE_H_
