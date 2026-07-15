// Layer 1 of the mimalloc integration: global operator new/delete routed
// through mimalloc, now with per-category byte tracking (memory_tracker.h).
// Global operators must be defined once per binary, so this file is compiled
// into each executable by rx_enable_mimalloc; it replaces the former
// mimalloc-new-delete.h include on Windows and libstdc++'s malloc-backed
// operators on POSIX (where layer 2, the whole-archive static mimalloc, still
// interposes raw malloc/free for third-party code; those allocations use
// mimalloc but are not assigned to a tracker category).
//
// Each allocation carries a small footer with its owning category. The user
// pointer stays equal to the mimalloc block pointer, so untracked allocations
// crossing a shared-library boundary can still be deleted safely.
#if defined(RX_MIMALLOC)

#include <mimalloc.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

#include "core/memory/memory_tracker.h"

namespace {

// Object files given to the linker are always linked whole, so this
// initializer runs in any binary that compiles this TU.
struct TrackingMarker {
  TrackingMarker() { rx::mem::detail::MarkTrackingActive(); }
} g_tracking_marker;

struct AllocationFooter {
  std::uintptr_t cookie;
  std::uintptr_t inverse_cookie;
  std::size_t usable_size;
  rx::mem::Category category;
};

constexpr std::uintptr_t kFooterMagic = static_cast<std::uintptr_t>(0xd6e8feb86659fd93ULL);

std::uintptr_t FooterCookie(void* pointer, std::size_t usable_size,
                            rx::mem::Category category) {
  return kFooterMagic ^ reinterpret_cast<std::uintptr_t>(pointer) ^ usable_size ^ category;
}

void* Allocate(std::size_t size, std::size_t alignment, bool aligned, bool nothrow) {
  if (size > std::numeric_limits<std::size_t>::max() - sizeof(AllocationFooter)) {
    if (nothrow) return nullptr;
    throw std::bad_alloc();
  }

  const std::size_t total = size + sizeof(AllocationFooter);
  void* pointer = nullptr;
  if (aligned) {
    pointer = nothrow ? mi_new_aligned_nothrow(total, alignment) : mi_new_aligned(total, alignment);
  } else {
    pointer = nothrow ? mi_new_nothrow(total) : mi_new(total);
  }
  if (!pointer) {
    if (nothrow) return nullptr;
    throw std::bad_alloc();
  }

  // _FORTIFY_SOURCE sizes the block from mi_new's alloc_size attribute (the
  // requested total), but the footer sits at the end of the *usable* block,
  // which size-class rounding can push past that bound. Hide the pointer's
  // provenance so __memcpy_chk cannot derive the too-small bound.
  __asm__("" : "+r"(pointer));

  const std::size_t usable_size = mi_usable_size(pointer);
  const rx::mem::Category category = rx::mem::CurrentCategory();
  const std::uintptr_t cookie = FooterCookie(pointer, usable_size, category);
  const AllocationFooter footer{
      .cookie = cookie,
      .inverse_cookie = ~cookie,
      .usable_size = usable_size,
      .category = category,
  };
  std::memcpy(static_cast<unsigned char*>(pointer) + usable_size - sizeof(footer), &footer,
              sizeof(footer));
  rx::mem::detail::TrackAlloc(category, usable_size);
  return pointer;
}

void Deallocate(void* pointer) noexcept {
  if (!pointer) return;
  const std::size_t usable_size = mi_usable_size(pointer);
  if (usable_size >= sizeof(AllocationFooter)) {
    AllocationFooter footer;
    std::memcpy(&footer, static_cast<unsigned char*>(pointer) + usable_size - sizeof(footer),
                sizeof(footer));
    const std::uintptr_t expected = FooterCookie(pointer, usable_size, footer.category);
    if (footer.usable_size == usable_size && footer.cookie == expected &&
        footer.inverse_cookie == ~expected && footer.category < rx::mem::kMaxCategories) {
      rx::mem::detail::TrackFree(footer.category, usable_size);
      const AllocationFooter cleared{};
      std::memcpy(static_cast<unsigned char*>(pointer) + usable_size - sizeof(cleared), &cleared,
                  sizeof(cleared));
    }
  }
  mi_free(pointer);
}

}  // namespace

void* operator new(std::size_t n) { return Allocate(n, 0, false, false); }
void* operator new[](std::size_t n) { return Allocate(n, 0, false, false); }
void* operator new(std::size_t n, std::align_val_t align) {
  return Allocate(n, static_cast<std::size_t>(align), true, false);
}
void* operator new[](std::size_t n, std::align_val_t align) {
  return Allocate(n, static_cast<std::size_t>(align), true, false);
}

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  return Allocate(n, 0, false, true);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return Allocate(n, 0, false, true);
}
void* operator new(std::size_t n, std::align_val_t align, const std::nothrow_t&) noexcept {
  return Allocate(n, static_cast<std::size_t>(align), true, true);
}
void* operator new[](std::size_t n, std::align_val_t align, const std::nothrow_t&) noexcept {
  return Allocate(n, static_cast<std::size_t>(align), true, true);
}

void operator delete(void* p) noexcept { Deallocate(p); }
void operator delete[](void* p) noexcept { Deallocate(p); }
void operator delete(void* p, std::size_t) noexcept { Deallocate(p); }
void operator delete[](void* p, std::size_t) noexcept { Deallocate(p); }
void operator delete(void* p, std::align_val_t) noexcept { Deallocate(p); }
void operator delete[](void* p, std::align_val_t) noexcept { Deallocate(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { Deallocate(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { Deallocate(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Deallocate(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Deallocate(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  Deallocate(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  Deallocate(p);
}

#endif  // RX_MIMALLOC
