#include "core/memory/frame_arena.h"

#include <cassert>
#include <cstdint>
#include <new>

namespace rx::mem {

namespace {
constexpr size_t kArenaAlign = 64;

size_t AlignUp(size_t value, size_t align) { return (value + align - 1) & ~(align - 1); }
}  // namespace

FrameArena::~FrameArena() { Shutdown(); }

void FrameArena::Init(size_t capacity_bytes) {
  Shutdown();
  capacity_ = AlignUp(capacity_bytes, kArenaAlign);
  base_ = static_cast<u8*>(::operator new(capacity_, std::align_val_t{kArenaAlign}));
}

void FrameArena::Shutdown() {
  Reset();
  if (base_) ::operator delete(base_, std::align_val_t{kArenaAlign});
  base_ = nullptr;
  capacity_ = 0;
  high_water_ = 0;
}

void* FrameArena::Alloc(size_t size, size_t align) {
  assert(align != 0 && (align & (align - 1)) == 0);
  const uintptr_t base_address = reinterpret_cast<uintptr_t>(base_);
  const uintptr_t current = base_address + offset_;
  const uintptr_t aligned_address = AlignUp(current, align);
  const size_t aligned = static_cast<size_t>(aligned_address - base_address);
  if (base_ && aligned <= capacity_ && size <= capacity_ - aligned) {
    offset_ = aligned + size;
    if (offset_ > high_water_) high_water_ = offset_;
    return reinterpret_cast<void*>(aligned_address);
  }
  const size_t overflow_align = align > kArenaAlign ? align : kArenaAlign;
  void* block = ::operator new(size, std::align_val_t{overflow_align});
  overflow_.push_back({block, overflow_align});
  ++overflow_allocs_;
  overflow_bytes_ += size;
  return block;
}

void FrameArena::Reset() {
  offset_ = 0;
  for (auto [block, align] : overflow_) ::operator delete(block, std::align_val_t{align});
  overflow_.clear();
  overflow_bytes_ = 0;
}

FrameArena::Stats FrameArena::stats() const {
  return Stats{
      .capacity_bytes = capacity_,
      .offset_bytes = offset_,
      .high_water_bytes = high_water_,
      .overflow_allocs = overflow_allocs_,
      .overflow_bytes = overflow_bytes_,
  };
}

FrameArena& MainFrameArena() {
  static FrameArena arena;
  return arena;
}

}  // namespace rx::mem
