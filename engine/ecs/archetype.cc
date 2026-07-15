#include "ecs/archetype.h"

#include <cassert>
#include <new>

#include "core/memory/chunk_pool.h"

namespace rx::ecs {

namespace {

u32 AlignUp(u32 value, u32 align) { return (value + align - 1) & ~(align - 1); }

}  // namespace

Archetype::Archetype(Signature signature) : signature_(std::move(signature)) {
  columns_.reserve(signature_.size());
  u32 total_stride = 0;
  u32 max_align = 1;
  for (ComponentId id : signature_) {
    const ComponentInfo& info = GetComponentInfo(id);
    columns_.push_back(Column{.id = id, .stride = info.size, .chunk_offset = 0});
    total_stride += info.size;
    if (info.align > max_align) max_align = info.align;
  }
  if (columns_.empty()) return;  // the empty archetype stores no component data

  // SoA layout within a chunk: find the largest row count whose column arrays
  // (each aligned to its component) fit the pool chunk. Padding is at most a
  // few cachelines, so the guess converges in a couple of iterations.
  const u32 pool_bytes = static_cast<u32>(mem::ChunkPool::kChunkSize);
  auto layout_bytes = [&](u32 rows) {
    u32 offset = 0;
    for (auto& column : columns_) {
      offset = AlignUp(offset, GetComponentInfo(column.id).align);
      column.chunk_offset = offset;
      offset += rows * column.stride;
    }
    return offset;
  };

  u32 rows = pool_bytes / total_stride;
  while (rows > 1 && layout_bytes(rows) > pool_bytes) --rows;
  if (rows == 0) rows = 1;

  pooled_ = max_align <= mem::ChunkPool::kChunkAlign && layout_bytes(rows) <= pool_bytes;
  rows_per_chunk_ = pooled_ ? rows : 1;
  chunk_bytes_ = pooled_ ? pool_bytes : layout_bytes(1);
  chunk_align_ = max_align > mem::ChunkPool::kChunkAlign
                     ? max_align
                     : static_cast<u32>(mem::ChunkPool::kChunkAlign);
  layout_bytes(rows_per_chunk_);  // bake the final chunk_offsets
}

Archetype::~Archetype() {
  for (auto& column : columns_) {
    const ComponentInfo& info = GetComponentInfo(column.id);
    for (u32 row = 0; row < row_count(); ++row) {
      info.destruct(RowAddress(column, row));
    }
  }
  for (void* chunk : chunks_) ReleaseChunk(chunk);
}

void* Archetype::AcquireChunk() {
  if (pooled_) return mem::GlobalChunkPool().Acquire();
  return ::operator new(chunk_bytes_, std::align_val_t{chunk_align_});
}

void Archetype::ReleaseChunk(void* chunk) {
  if (pooled_) {
    mem::GlobalChunkPool().Release(chunk);
  } else {
    ::operator delete(chunk, std::align_val_t{chunk_align_});
  }
}

u8* Archetype::RowAddress(const Column& column, u32 row) {
  u8* chunk = static_cast<u8*>(chunks_[row / rows_per_chunk_]);
  return chunk + column.chunk_offset + static_cast<size_t>(row % rows_per_chunk_) * column.stride;
}

u32 Archetype::AddRow(Entity entity) {
  const u32 row = row_count();
  if (!columns_.empty() && row == chunk_count() * rows_per_chunk_) {
    chunks_.push_back(AcquireChunk());
  }
  entities_.push_back(entity);
  return row;
}

Entity Archetype::SwapRemoveRow(u32 row) {
  const u32 last = row_count() - 1;
  for (auto& column : columns_) {
    const ComponentInfo& info = GetComponentInfo(column.id);
    u8* dst = RowAddress(column, row);
    info.destruct(dst);
    if (row != last) {
      u8* src = RowAddress(column, last);
      info.move_construct(dst, src);
      info.destruct(src);
    }
  }
  Entity moved = kInvalidEntity;
  if (row != last) {
    moved = entities_[last];
    entities_[row] = moved;
  }
  entities_.pop_back();
  // Return a fully vacated tail chunk to the pool.
  if (!columns_.empty() && last % rows_per_chunk_ == 0 && !chunks_.empty()) {
    ReleaseChunk(chunks_.back());
    chunks_.pop_back();
  }
  return moved;
}

void* Archetype::ComponentAt(ComponentId id, u32 row) {
  const int index = ColumnIndex(id);
  if (index < 0) return nullptr;
  return RowAddress(columns_[static_cast<size_t>(index)], row);
}

void* Archetype::ChunkColumnData(u32 chunk, int column_index) {
  const Column& column = columns_[static_cast<size_t>(column_index)];
  return static_cast<u8*>(chunks_[chunk]) + column.chunk_offset;
}

u32 Archetype::ChunkRowCount(u32 chunk) const {
  const u32 begin = chunk * rows_per_chunk_;
  const u32 count = row_count();
  return count - begin < rows_per_chunk_ ? count - begin : rows_per_chunk_;
}

int Archetype::ColumnIndex(ComponentId id) const {
  auto it = std::lower_bound(signature_.begin(), signature_.end(), id);
  if (it == signature_.end() || *it != id) return -1;
  return static_cast<int>(it - signature_.begin());
}

}  // namespace rx::ecs
