// Layer 1 of the mimalloc integration: global operator new/delete routed
// through mimalloc, now with per-category byte tracking (memory_tracker.h).
// Global operators must be defined once per binary, so this file is compiled
// into each executable by rx_enable_mimalloc; it replaces the former
// mimalloc-new-delete.h include on Windows and libstdc++'s malloc-backed
// operators on POSIX (where layer 2, the whole-archive static mimalloc, still
// interposes raw malloc/free for third-party code — those allocations land in
// mimalloc but only in the tracker's process totals, not a category).
//
// Tracking uses mi_usable_size on both sides so alloc/free deltas cancel
// exactly regardless of size-class rounding.
#if defined(RX_MIMALLOC)

#include <mimalloc.h>

#include <cstddef>
#include <new>

#include "core/memory/memory_tracker.h"

namespace {

// Object files given to the linker are always linked whole, so this
// initializer runs in any binary that compiles this TU.
struct TrackingMarker {
  TrackingMarker() { rx::mem::detail::MarkTrackingActive(); }
} g_tracking_marker;

inline void* TrackNew(void* p) {
  if (p) rx::mem::TrackAlloc(mi_usable_size(p));
  return p;
}

inline void TrackDelete(void* p) {
  if (p) rx::mem::TrackFree(mi_usable_size(p));
}

}  // namespace

void* operator new(std::size_t n) { return TrackNew(mi_new(n)); }
void* operator new[](std::size_t n) { return TrackNew(mi_new(n)); }
void* operator new(std::size_t n, std::align_val_t align) {
  return TrackNew(mi_new_aligned(n, static_cast<std::size_t>(align)));
}
void* operator new[](std::size_t n, std::align_val_t align) {
  return TrackNew(mi_new_aligned(n, static_cast<std::size_t>(align)));
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  return TrackNew(mi_new_nothrow(n));
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return TrackNew(mi_new_nothrow(n));
}
void* operator new(std::size_t n, std::align_val_t align, const std::nothrow_t&) noexcept {
  return TrackNew(mi_new_aligned_nothrow(n, static_cast<std::size_t>(align)));
}
void* operator new[](std::size_t n, std::align_val_t align, const std::nothrow_t&) noexcept {
  return TrackNew(mi_new_aligned_nothrow(n, static_cast<std::size_t>(align)));
}

void operator delete(void* p) noexcept {
  TrackDelete(p);
  mi_free(p);
}
void operator delete[](void* p) noexcept {
  TrackDelete(p);
  mi_free(p);
}
void operator delete(void* p, std::size_t n) noexcept {
  TrackDelete(p);
  mi_free_size(p, n);
}
void operator delete[](void* p, std::size_t n) noexcept {
  TrackDelete(p);
  mi_free_size(p, n);
}
void operator delete(void* p, std::align_val_t align) noexcept {
  TrackDelete(p);
  mi_free_aligned(p, static_cast<std::size_t>(align));
}
void operator delete[](void* p, std::align_val_t align) noexcept {
  TrackDelete(p);
  mi_free_aligned(p, static_cast<std::size_t>(align));
}
void operator delete(void* p, std::size_t n, std::align_val_t align) noexcept {
  TrackDelete(p);
  mi_free_size_aligned(p, n, static_cast<std::size_t>(align));
}
void operator delete[](void* p, std::size_t n, std::align_val_t align) noexcept {
  TrackDelete(p);
  mi_free_size_aligned(p, n, static_cast<std::size_t>(align));
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  TrackDelete(p);
  mi_free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  TrackDelete(p);
  mi_free(p);
}
void operator delete(void* p, std::align_val_t align, const std::nothrow_t&) noexcept {
  TrackDelete(p);
  mi_free_aligned(p, static_cast<std::size_t>(align));
}
void operator delete[](void* p, std::align_val_t align, const std::nothrow_t&) noexcept {
  TrackDelete(p);
  mi_free_aligned(p, static_cast<std::size_t>(align));
}

#endif  // RX_MIMALLOC
