#ifndef RX_RENDER_RT_SLOT_TRACKER_H_
#define RX_RENDER_RT_SLOT_TRACKER_H_

#include "core/types.h"

namespace rx::render {

// Pure bookkeeping for the ping-pong TLAS slots (RayTracingContext::kSlots).
// The async-TLAS path builds slot N this frame and reads the slot built last
// frame, so consumers must be told when the slot they are about to read is not
// actually a valid, current build:
//   * RT enabled after raster-only frames -> the previous slot was never built,
//     binding it yields a null acceleration structure (validation error).
//   * a mesh/BLAS replaced (renderer WaitIdle + RemoveBlas) -> slots built
//     before the replace still reference the freed BLAS device addresses, so
//     tracing them risks stale reads / device loss.
//
// Two independent facts per slot: whether it was ever built this session, and
// the BLAS-set revision it was built against. Removing/replacing a BLAS bumps
// the global revision, retiring every slot built before it. No device
// dependency on purpose -- the selection logic is unit-tested off-GPU
// (rt_slot_tracker_test).
struct TlasSlotTracker {
  static constexpr u32 kSlots = 3;

  // Record that `slot` was (re)built this frame against the current BLAS set.
  void MarkBuilt(u32 slot) {
    built_[slot] = true;
    slot_revision_[slot] = revision_;
  }

  // A BLAS was removed/replaced: every previously built slot now references a
  // structure that no longer exists. Bump the revision so they read as invalid
  // until rebuilt against the new set.
  void InvalidateBuilds() { ++revision_; }

  // A slot may be read iff it was built at least once and against the current
  // BLAS revision.
  bool Valid(u32 slot) const { return built_[slot] && slot_revision_[slot] == revision_; }

  struct Selection {
    u32 build_slot;  // the slot to (re)build this frame
    u32 read_slot;   // the slot consumers should read this frame
    bool async;      // whether the build may run a frame ahead on the async queue
  };

  // Choose slots for `frame_index`. The async path builds the current slot a
  // frame ahead while reading the previous one; if that previous slot is not
  // valid (never built, or retired by a BLAS change), fall back to a synchronous
  // build+read of the current slot, which this frame's build pass primes.
  Selection Select(u32 frame_index, bool want_async) const {
    const u32 build = frame_index % kSlots;
    const u32 prev = (frame_index + kSlots - 1) % kSlots;
    if (want_async && Valid(prev)) return {build, prev, true};
    return {build, build, false};
  }

  bool built_[kSlots] = {};
  u32 slot_revision_[kSlots] = {};
  u32 revision_ = 0;
};

}  // namespace rx::render

#endif  // RX_RENDER_RT_SLOT_TRACKER_H_
