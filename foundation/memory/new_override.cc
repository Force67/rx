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

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>

#include "base/memory/mem_ops.h"
#include "base/standard_streams.h"
#include "foundation/memory/memory_tracker.h"

namespace {

// Object files given to the linker are always linked whole, so this
// initializer runs in any binary that compiles this TU.
struct TrackingMarker {
  TrackingMarker() { rx::mem::detail::MarkTrackingActive(); }
} g_tracking_marker;

struct AllocationFooter {
  uintptr_t cookie;
  uintptr_t inverse_cookie;
  size_t usable_size;
  rx::mem::Category category;
};

constexpr uintptr_t kFooterMagic = static_cast<uintptr_t>(0xd6e8feb86659fd93ULL);

uintptr_t FooterCookie(void* pointer, size_t usable_size,
                            rx::mem::Category category) {
  return kFooterMagic ^ reinterpret_cast<uintptr_t>(pointer) ^ usable_size ^ category;
}

// Exceptions are not used, so the throwing forms of operator new end the
// process on exhaustion instead of raising bad_alloc; the nothrow forms return
// null as their contract says.
[[noreturn]] void OutOfMemory() {
  static const char kMessage[] = "rx: out of memory in operator new\n";
  base::WriteStandardError(kMessage, sizeof(kMessage) - 1);
  ::abort();
}

void* Allocate(size_t size, size_t alignment, bool aligned, bool nothrow) {
  if (size > SIZE_MAX - sizeof(AllocationFooter)) {
    if (nothrow) return nullptr;
    OutOfMemory();
  }

  const size_t total = size + sizeof(AllocationFooter);
  void* pointer = aligned ? mi_new_aligned_nothrow(total, alignment) : mi_new_nothrow(total);
  if (!pointer) {
    if (nothrow) return nullptr;
    OutOfMemory();
  }

  // _FORTIFY_SOURCE sizes the block from mi_new's alloc_size attribute (the
  // requested total), but the footer sits at the end of the *usable* block,
  // which size-class rounding can push past that bound. Hide the pointer's
  // provenance so __memcpy_chk cannot derive the too-small bound.
  __asm__("" : "+r"(pointer));

  const size_t usable_size = mi_usable_size(pointer);
  const rx::mem::Category category = rx::mem::CurrentCategory();
  const uintptr_t cookie = FooterCookie(pointer, usable_size, category);
  const AllocationFooter footer{
      .cookie = cookie,
      .inverse_cookie = ~cookie,
      .usable_size = usable_size,
      .category = category,
  };
  base::MemCopy(static_cast<unsigned char*>(pointer) + usable_size - sizeof(footer), &footer,
              sizeof(footer));
  rx::mem::detail::TrackAlloc(category, usable_size);
  return pointer;
}

void Deallocate(void* pointer) noexcept {
  if (!pointer) return;
  const size_t usable_size = mi_usable_size(pointer);
  if (usable_size >= sizeof(AllocationFooter)) {
    AllocationFooter footer;
    base::MemCopy(&footer, static_cast<unsigned char*>(pointer) + usable_size - sizeof(footer),
                sizeof(footer));
    const uintptr_t expected = FooterCookie(pointer, usable_size, footer.category);
    if (footer.usable_size == usable_size && footer.cookie == expected &&
        footer.inverse_cookie == ~expected && footer.category < rx::mem::kMaxCategories) {
      rx::mem::detail::TrackFree(footer.category, usable_size);
      const AllocationFooter cleared{};
      base::MemCopy(static_cast<unsigned char*>(pointer) + usable_size - sizeof(cleared), &cleared,
                  sizeof(cleared));
    }
  }
  mi_free(pointer);
}

}  // namespace

void* operator new(size_t n) { return Allocate(n, 0, false, false); }
void* operator new[](size_t n) { return Allocate(n, 0, false, false); }
void* operator new(size_t n, std::align_val_t align) {
  return Allocate(n, static_cast<size_t>(align), true, false);
}
void* operator new[](size_t n, std::align_val_t align) {
  return Allocate(n, static_cast<size_t>(align), true, false);
}

void* operator new(size_t n, const std::nothrow_t&) noexcept {
  return Allocate(n, 0, false, true);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
  return Allocate(n, 0, false, true);
}
void* operator new(size_t n, std::align_val_t align, const std::nothrow_t&) noexcept {
  return Allocate(n, static_cast<size_t>(align), true, true);
}
void* operator new[](size_t n, std::align_val_t align, const std::nothrow_t&) noexcept {
  return Allocate(n, static_cast<size_t>(align), true, true);
}

void operator delete(void* p) noexcept { Deallocate(p); }
void operator delete[](void* p) noexcept { Deallocate(p); }
void operator delete(void* p, size_t) noexcept { Deallocate(p); }
void operator delete[](void* p, size_t) noexcept { Deallocate(p); }
void operator delete(void* p, std::align_val_t) noexcept { Deallocate(p); }
void operator delete[](void* p, std::align_val_t) noexcept { Deallocate(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { Deallocate(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { Deallocate(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Deallocate(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Deallocate(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  Deallocate(p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
  Deallocate(p);
}

#endif  // RX_MIMALLOC
