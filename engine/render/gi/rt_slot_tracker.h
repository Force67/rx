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
// A slot records the frame and BLAS-set revision of its last completed build.
// Removing/replacing a BLAS bumps the global revision, retiring every slot built
// before it. No device dependency on purpose -- the selection logic is
// unit-tested off-GPU (rt_slot_tracker_test).
struct TlasSlotTracker {
  // Four divides the u32 frame-counter period, so the modulo slot sequence
  // remains continuous when frame_index wraps to zero.
  static constexpr u32 kSlots = 4;

  // Record that `slot` was (re)built this frame against the current BLAS set.
  void MarkBuilt(u32 slot, u32 frame_index) {
    built_[slot] = true;
    built_frame_[slot] = frame_index;
    slot_revision_[slot] = revision_;
  }

  // A reservation or record-time invariant failed for one slot. Its resources
  // may still exist, but consumers must use the empty fallback until a rebuild.
  void Invalidate(u32 slot) { built_[slot] = false; }

  // A BLAS was removed/replaced: every previously built slot now references a
  // structure that no longer exists. Bump the revision so they read as invalid
  // until rebuilt against the new set.
  void InvalidateBuilds() { ++revision_; }

  // A slot may be read iff it was built at least once and against the current
  // BLAS revision.
  bool Valid(u32 slot) const { return built_[slot] && slot_revision_[slot] == revision_; }

  bool ValidForFrame(u32 slot, u32 frame_index) const {
    return Valid(slot) && built_frame_[slot] == frame_index;
  }

  struct Selection {
    u32 build_slot;  // the slot to (re)build this frame
    u32 read_slot;   // the slot consumers should read this frame
    bool async;      // whether the build may run a frame ahead on the async queue
  };

  // Choose slots for `frame_index`. The async path builds the current slot a
  // frame ahead while reading the previous one; if that previous slot is not
  // valid (never built, or retired by a BLAS change), fall back to a synchronous
  // build+read of the current slot, which this frame's build pass primes.
  Selection Select(u32 frame_index, bool want_async) {
    // The wrapped frame value is also the freshness token. Retire the previous
    // epoch before frame numbers can alias old build stamps.
    if (have_frame_ && frame_index < last_frame_) {
      for (u32 slot = 0; slot < kSlots; ++slot) built_[slot] = false;
    }
    last_frame_ = frame_index;
    have_frame_ = true;
    const u32 build = frame_index % kSlots;
    const u32 prev = (build + kSlots - 1) % kSlots;
    if (want_async && frame_index > 0 && ValidForFrame(prev, frame_index - 1))
      return {build, prev, true};
    return {build, build, false};
  }

  bool built_[kSlots] = {};
  u32 built_frame_[kSlots] = {};
  u32 slot_revision_[kSlots] = {};
  u32 revision_ = 0;
  u32 last_frame_ = 0;
  bool have_frame_ = false;
};

}  // namespace rx::render

#endif  // RX_RENDER_RT_SLOT_TRACKER_H_
