#ifndef RX_CORE_MEMORY_CHUNK_POOL_H_
#define RX_CORE_MEMORY_CHUNK_POOL_H_

#include <cstddef>
#include <mutex>
#include <vector>

#include "core/export.h"
#include "core/types.h"

namespace rx::mem {

// Pool of fixed 16 KiB blocks, the backing store for ECS archetype chunks
// (and any other consumer with chunk-shaped lifetimes). One block size means
// zero external fragmentation and O(1) acquire/release; memory is carved from
// 1 MiB slabs that are retained for the process lifetime, so long sessions
// reach a steady state instead of fragmenting the general heap.
//
// Thread-safe (a mutex around the freelist); acquire/release happens per
// 16 KiB of entity data, not per allocation, so contention is negligible.
class RX_CORE_EXPORT ChunkPool {
 public:
  static constexpr size_t kChunkSize = 16 * 1024;
  static constexpr size_t kChunkAlign = 64;

  ChunkPool() = default;
  ~ChunkPool();

  ChunkPool(const ChunkPool&) = delete;
  ChunkPool& operator=(const ChunkPool&) = delete;

  void* Acquire();
  void Release(void* chunk);

  // Pre-carves slabs until at least `chunk_count` chunks exist (used by the
  // memory config to front-load the expected working set at startup).
  void Reserve(size_t chunk_count);

  struct Stats {
    size_t total_chunks = 0;
    size_t free_chunks = 0;
  };
  Stats stats() const;

 private:
  static constexpr size_t kChunksPerSlab = 64;  // 1 MiB slabs

  void AddSlabLocked();

  mutable std::mutex mutex_;
  std::vector<void*> slabs_;
  std::vector<void*> free_;
  size_t total_ = 0;
};

RX_CORE_EXPORT ChunkPool& GlobalChunkPool();

}  // namespace rx::mem

#endif  // RX_CORE_MEMORY_CHUNK_POOL_H_
