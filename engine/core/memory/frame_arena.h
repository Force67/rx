#ifndef RX_CORE_MEMORY_FRAME_ARENA_H_
#define RX_CORE_MEMORY_FRAME_ARENA_H_

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/export.h"
#include "core/types.h"

namespace rx::mem {

// Linear (bump) allocator for allocations that live exactly one frame:
// gather lists, scratch buffers, pass-closure captures. Alloc is a pointer
// bump; Reset at the top of the frame reclaims everything at once. Nothing is
// destructed — only trivially-destructible payloads belong here.
//
// Single-threaded by design: owned by the frame loop (app::Host resets it in
// RunFrame). Worker threads must not allocate from it.
//
// Requests that do not fit fall back to the heap and are freed on Reset; the
// overflow counters in Stats flag an undersized arena so the budget config
// can be raised instead of silently degrading.
class RX_CORE_EXPORT FrameArena {
 public:
  FrameArena() = default;
  ~FrameArena();

  FrameArena(const FrameArena&) = delete;
  FrameArena& operator=(const FrameArena&) = delete;

  void Init(size_t capacity_bytes);
  void Shutdown();

  void* Alloc(size_t size, size_t align);

  // Uninitialized storage for `count` Ts; the arena never runs destructors.
  template <typename T>
  T* AllocArray(size_t count) {
    static_assert(std::is_trivially_destructible_v<T>);
    return static_cast<T*>(Alloc(count * sizeof(T), alignof(T)));
  }

  // Reclaims all frame allocations (and frees any overflow blocks).
  void Reset();

  struct Stats {
    size_t capacity_bytes = 0;
    size_t offset_bytes = 0;      // used this frame so far
    size_t high_water_bytes = 0;  // max offset seen across all frames
    u64 overflow_allocs = 0;      // lifetime count of fall-back heap allocs
    size_t overflow_bytes = 0;    // overflow bytes in the current frame
  };
  Stats stats() const;

 private:
  u8* base_ = nullptr;
  size_t capacity_ = 0;
  size_t offset_ = 0;
  size_t high_water_ = 0;
  std::vector<std::pair<void*, size_t>> overflow_;  // block, alignment
  u64 overflow_allocs_ = 0;
  size_t overflow_bytes_ = 0;
};

// The main-thread frame arena, reset by app::Host at the top of each frame.
RX_CORE_EXPORT FrameArena& MainFrameArena();

}  // namespace rx::mem

#endif  // RX_CORE_MEMORY_FRAME_ARENA_H_
