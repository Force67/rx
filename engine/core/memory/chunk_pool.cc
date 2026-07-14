#include "core/memory/chunk_pool.h"

#include <new>

namespace rx::mem {

ChunkPool::~ChunkPool() {
  for (void* slab : slabs_) ::operator delete(slab, std::align_val_t{kChunkAlign});
}

void ChunkPool::AddSlabLocked() {
  void* slab = ::operator new(kChunksPerSlab * kChunkSize, std::align_val_t{kChunkAlign});
  slabs_.push_back(slab);
  for (size_t i = 0; i < kChunksPerSlab; ++i) {
    free_.push_back(static_cast<u8*>(slab) + i * kChunkSize);
  }
  total_ += kChunksPerSlab;
}

void* ChunkPool::Acquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_.empty()) AddSlabLocked();
  void* chunk = free_.back();
  free_.pop_back();
  return chunk;
}

void ChunkPool::Release(void* chunk) {
  if (!chunk) return;
  std::lock_guard<std::mutex> lock(mutex_);
  free_.push_back(chunk);
}

void ChunkPool::Reserve(size_t chunk_count) {
  std::lock_guard<std::mutex> lock(mutex_);
  while (total_ < chunk_count) AddSlabLocked();
}

ChunkPool::Stats ChunkPool::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return Stats{.total_chunks = total_, .free_chunks = free_.size()};
}

ChunkPool& GlobalChunkPool() {
  static ChunkPool pool;
  return pool;
}

}  // namespace rx::mem
