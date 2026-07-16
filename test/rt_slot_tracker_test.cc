// Pure-logic unit test for the async-TLAS slot tracker (no GPU): build/read slot
// selection, the unbuilt-slot synchronous fallback, and BLAS-revision
// invalidation. Mirrors the discipline RayTracingContext applies around
// BuildTlas / RemoveBlas so the crash cases the reviewer flagged (binding an
// unbuilt slot, reading a slot whose BLASes were replaced) are guarded off-GPU.
#include "render/gi/rt_slot_tracker.h"

#include <cstdio>

using namespace rx;
using namespace rx::render;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

// Simulate the renderer/RayTracingContext loop for one frame: select slots for
// `frame`, then record the build (MarkBuilt) exactly as BuildTlas does.
TlasSlotTracker::Selection RunFrame(TlasSlotTracker& t, u32 frame, bool want_async) {
  TlasSlotTracker::Selection s = t.Select(frame, want_async);
  t.MarkBuilt(s.build_slot);  // BuildTlas always builds the current slot
  return s;
}

}  // namespace

int main() {
  constexpr u32 kSlots = TlasSlotTracker::kSlots;

  // --- RT enabled from frame 0: first frame cannot read an async prev slot ---
  {
    TlasSlotTracker t;
    // Frame 0: nothing built yet, so even with async wanted we must build+read
    // the current slot synchronously (binding the unbuilt prev slot = null AS).
    TlasSlotTracker::Selection s0 = RunFrame(t, 0, /*want_async=*/true);
    CHECK(!s0.async);
    CHECK(s0.read_slot == s0.build_slot);
    CHECK(s0.build_slot == 0);
    // Frame 1: the previous slot (0) is now a valid build -> async engages and
    // reads it while building slot 1 ahead.
    TlasSlotTracker::Selection s1 = RunFrame(t, 1, true);
    CHECK(s1.async);
    CHECK(s1.build_slot == 1 % kSlots);
    CHECK(s1.read_slot == 0);
    // Steady state stays async.
    TlasSlotTracker::Selection s2 = RunFrame(t, 2, true);
    CHECK(s2.async && s2.read_slot == 1);
  }

  // --- want_async false always builds+reads the current slot (sync path) ---
  {
    TlasSlotTracker t;
    for (u32 f = 0; f < 5; ++f) {
      TlasSlotTracker::Selection s = RunFrame(t, f, /*want_async=*/false);
      CHECK(!s.async);
      CHECK(s.read_slot == s.build_slot);
      CHECK(s.build_slot == f % kSlots);
    }
  }

  // --- RT enabled only after several raster-only frames ---
  {
    TlasSlotTracker t;
    // Frames 0..4 render raster-only: no TLAS is built (BuildTlas not called),
    // so the tracker records nothing. The frame counter still advances.
    const u32 enable_frame = 5;
    // First RT frame: the async prev slot was never built -> sync fallback.
    TlasSlotTracker::Selection s = RunFrame(t, enable_frame, /*want_async=*/true);
    CHECK(!s.async);
    CHECK(s.read_slot == s.build_slot);
    // Next frame the just-built slot is a valid prev -> async resumes.
    TlasSlotTracker::Selection s2 = RunFrame(t, enable_frame + 1, true);
    CHECK(s2.async);
    CHECK(s2.read_slot == enable_frame % kSlots);
  }

  // --- BLAS replaced (RemoveBlas) invalidates every prior slot ---
  {
    TlasSlotTracker t;
    // Prime all slots over several async frames so every slot is a valid build.
    for (u32 f = 0; f < 2 * kSlots; ++f) RunFrame(t, f, true);
    for (u32 s = 0; s < kSlots; ++s) CHECK(t.Valid(s));

    // A mesh replace destroys BLASes and bumps the revision: no slot is valid,
    // so the next frame must NOT read the (stale, freed-BLAS) prev slot.
    t.InvalidateBuilds();
    for (u32 s = 0; s < kSlots; ++s) CHECK(!t.Valid(s));
    TlasSlotTracker::Selection s = RunFrame(t, 2 * kSlots, /*want_async=*/true);
    CHECK(!s.async);
    CHECK(s.read_slot == s.build_slot);
    // The rebuilt slot is valid again; async resumes the following frame.
    TlasSlotTracker::Selection s2 = RunFrame(t, 2 * kSlots + 1, true);
    CHECK(s2.async);
  }

  // --- a slot built under an older revision never reads as valid ---
  {
    TlasSlotTracker t;
    t.MarkBuilt(0);
    CHECK(t.Valid(0));
    t.InvalidateBuilds();  // BLAS set changed
    CHECK(!t.Valid(0));    // slot 0's build predates the current revision
    t.MarkBuilt(0);        // rebuilt against the new set
    CHECK(t.Valid(0));
  }

  if (g_failures == 0) {
    std::printf("rt_slot_tracker_test: all checks passed\n");
    return 0;
  }
  std::printf("rt_slot_tracker_test: %d checks FAILED\n", g_failures);
  return 1;
}
