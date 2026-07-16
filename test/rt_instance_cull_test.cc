// Pure-logic unit test for the solid-angle TLAS instance culler (no GPU): the
// distance/solid-angle predicate, the time-sliced group sweep, teleport
// accept-all fallback and generation invalidation.
#include "render/gi/rt_instance_cull.h"

#include <cstdio>

#include "core/math.h"

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

Mat4 At(const Vec3& p) { return MakeTranslation(p); }

// Count set bytes in a visibility bitmask.
u32 CountVisible(const base::Vector<u8>& v) {
  u32 n = 0;
  for (u8 b : v) n += b ? 1 : 0;
  return n;
}

}  // namespace

int main() {
  const f32 start = 40.0f;
  const f32 angle = 0.004f;  // ~a 0.5 m radius object culls near 125 m

  // --- inline per-draw predicate ---
  {
    RtInstanceCuller c;
    c.Configure(true, start, angle);
    c.BeginFrame(Vec3{0, 0, 0});
    const Vec3 origin{0, 0, 0};
    // Near field: tiny object well inside the start distance stays.
    CHECK(c.DrawVisible(At(Vec3{0, 0, 10}), origin, 0.05f));
    // Beyond start, a small object below the angular threshold drops.
    CHECK(!c.DrawVisible(At(Vec3{0, 0, 200}), origin, 0.1f));  // 0.1/200 = 0.0005 < angle
    // Beyond start, a large object above the threshold stays.
    CHECK(c.DrawVisible(At(Vec3{0, 0, 200}), origin, 5.0f));   // 5/200 = 0.025 > angle
    // Right at the start distance everything is kept.
    CHECK(c.DrawVisible(At(Vec3{0, 0, start}), origin, 0.001f));
    // Unknown bounds (radius 0) are never culled, however far away.
    CHECK(c.DrawVisible(At(Vec3{0, 0, 100000}), origin, 0.0f));
  }

  // --- disabled culler keeps everything ---
  {
    RtInstanceCuller c;
    c.Configure(false, start, angle);
    c.BeginFrame(Vec3{0, 0, 0});
    CHECK(c.DrawVisible(At(Vec3{0, 0, 5000}), Vec3{0, 0, 0}, 0.001f));
    base::Vector<Mat4> xf;
    for (int i = 0; i < 10; ++i) xf.push_back(At(Vec3{0, 0, 5000.0f + i}));
    const auto& vis = c.UpdateGroup(0, 1, 0, {xf.data(), xf.size()}, Vec3{0, 0, 0}, 0.01f);
    CHECK(CountVisible(vis) == xf.size());
  }

  // --- time-sliced group sweep converges over ~kSweepFrames ---
  {
    RtInstanceCuller c;
    c.Configure(true, start, angle);
    // A big group of tiny far props that should all end up culled.
    base::Vector<Mat4> xf;
    const u32 kN = 5000;
    for (u32 i = 0; i < kN; ++i) xf.push_back(At(Vec3{f32(i % 50), 0, 500.0f + f32(i)}));
    const Vec3 eye{0, 0, 0};
    const f32 mesh_radius = 0.05f;  // tiny -> below threshold past start

    c.BeginFrame(eye);
    // First frame is accept-all (group just appeared): no holes.
    const auto& first = c.UpdateGroup(0, 1, 0, {xf.data(), xf.size()}, Vec3{0, 0, 0}, mesh_radius);
    CHECK(CountVisible(first) == kN);

    // Per-frame slice is bounded (never a full re-test of a huge group).
    const u32 expected_slice =
        (kN + RtInstanceCuller::kSweepFrames - 1) / RtInstanceCuller::kSweepFrames;
    CHECK(expected_slice < kN);

    // Run a full sweep; by kSweepFrames every tiny far prop is culled.
    u32 visible = kN;
    for (u32 f = 0; f < RtInstanceCuller::kSweepFrames + 2; ++f) {
      c.BeginFrame(eye);
      const auto& v = c.UpdateGroup(0, 1, 0, {xf.data(), xf.size()}, Vec3{0, 0, 0}, mesh_radius);
      visible = CountVisible(v);
    }
    CHECK(visible == 0);
  }

  // --- teleport falls back to accept-all ---
  {
    RtInstanceCuller c;
    c.Configure(true, start, angle);
    base::Vector<Mat4> xf;
    const u32 kN = 300;
    for (u32 i = 0; i < kN; ++i) xf.push_back(At(Vec3{0, 0, 500.0f + f32(i)}));
    const Vec3 eye{0, 0, 0};

    // Converge to fully culled first.
    for (u32 f = 0; f < RtInstanceCuller::kSweepFrames + 2; ++f) {
      c.BeginFrame(eye);
      c.UpdateGroup(0, 1, 0, {xf.data(), xf.size()}, Vec3{0, 0, 0}, 0.05f);
    }
    // Now teleport far away in a single frame -> accept-all restored.
    c.BeginFrame(Vec3{10000, 0, 0});
    const auto& v = c.UpdateGroup(0, 1, 0, {xf.data(), xf.size()}, Vec3{0, 0, 0}, 0.05f);
    CHECK(CountVisible(v) == kN);
  }

  // --- generation change invalidates stale state ---
  {
    RtInstanceCuller c;
    c.Configure(true, start, angle);
    base::Vector<Mat4> a;
    for (u32 i = 0; i < 100; ++i) a.push_back(At(Vec3{0, 0, 500.0f + f32(i)}));
    const Vec3 eye{0, 0, 0};
    for (u32 f = 0; f < RtInstanceCuller::kSweepFrames + 2; ++f) {
      c.BeginFrame(eye);
      c.UpdateGroup(3, 1, 0, {a.data(), a.size()}, Vec3{0, 0, 0}, 0.05f);
    }
    // Slot 3 reused for a new group (generation bumped): starts accept-all.
    base::Vector<Mat4> b;
    for (u32 i = 0; i < 100; ++i) b.push_back(At(Vec3{0, 0, 500.0f + f32(i)}));
    c.BeginFrame(eye);
    const auto& v = c.UpdateGroup(3, 2, 0, {b.data(), b.size()}, Vec3{0, 0, 0}, 0.05f);
    CHECK(CountVisible(v) == b.size());
  }

  // --- in-place transform update (revision bump) re-admits a moved instance ---
  {
    RtInstanceCuller c;
    c.Configure(true, start, angle);
    // One tiny prop far away plus filler; converge it to culled.
    base::Vector<Mat4> xf;
    const u32 kN = 200;
    for (u32 i = 0; i < kN; ++i) xf.push_back(At(Vec3{0, 0, 500.0f + f32(i)}));
    const Vec3 eye{0, 0, 0};
    for (u32 f = 0; f < RtInstanceCuller::kSweepFrames + 2; ++f) {
      c.BeginFrame(eye);
      c.UpdateGroup(7, 1, 0, {xf.data(), xf.size()}, Vec3{0, 0, 0}, 0.05f);
    }
    // Move instance 0 right next to the camera. Same group_id, same generation,
    // same count -- only the transforms (and the store's revision) change.
    xf[0] = At(Vec3{0, 0, 5});
    c.BeginFrame(eye);
    // Without the revision bump the stale bitmask still hides instance 0 (its
    // sweep index has not come back around); with it the group re-admits all.
    const auto& moved = c.UpdateGroup(7, 1, /*revision=*/1, {xf.data(), xf.size()},
                                      Vec3{0, 0, 0}, 0.05f);
    CHECK(moved[0] == 1);
    CHECK(CountVisible(moved) == kN);  // revision bump restarts accept-all
  }

  if (g_failures == 0) {
    std::printf("rt_instance_cull_test: all checks passed\n");
    return 0;
  }
  std::printf("rt_instance_cull_test: %d checks FAILED\n", g_failures);
  return 1;
}
