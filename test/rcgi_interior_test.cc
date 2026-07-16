// Pure-logic unit test for the RCGI leak/occlusion hardening helpers (no GPU):
// interior-volume point classification and the probe-relocation offset packing
// round trip. These mirror the GPU shader helpers (rcgi_common.hlsli) exactly,
// so the CPU test guards the classification predicate and the +-0.45-cell offset
// quantization the shaders rely on.
#include "render/gi/rcgi_interior.h"

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

bool Near(f32 a, f32 b, f32 eps) { return std::fabs(a - b) <= eps; }

}  // namespace

int main() {
  // --- interior-volume classification ---
  {
    InteriorVolume vols[2] = {
        {Vec3{-1, 0, -1}, Vec3{1, 3, 1}},     // a room around the origin
        {Vec3{10, 0, 10}, Vec3{14, 4, 14}},   // a second, disjoint room
    };
    // Inside the first box (incl. boundary), inside the second, and outside both.
    CHECK(PointInInteriorVolumes(vols, 2, Vec3{0, 1.5f, 0}));
    CHECK(PointInInteriorVolumes(vols, 2, Vec3{1, 3, 1}));       // corner is inside
    CHECK(PointInInteriorVolumes(vols, 2, Vec3{12, 2, 12}));
    CHECK(!PointInInteriorVolumes(vols, 2, Vec3{5, 1, 5}));      // doorway gap
    CHECK(!PointInInteriorVolumes(vols, 2, Vec3{0, 3.01f, 0}));  // just above the ceiling
    // count == 0 classifies everything as outdoor (the global-interior path uses
    // the flat ambient, not volumes).
    CHECK(!PointInInteriorVolumes(vols, 0, Vec3{0, 1.5f, 0}));
  }

  // --- relocation offset packing round trip (fraction of spacing) ---
  {
    // Zero offset survives exactly.
    CHECK(RcgiPackOffset(Vec3{0, 0, 0}) == RcgiPackOffset(Vec3{0, 0, 0}));
    Vec3 z = RcgiUnpackOffset(RcgiPackOffset(Vec3{0, 0, 0}));
    CHECK(Near(z.x, 0.0f, 1e-3f) && Near(z.y, 0.0f, 1e-3f) && Near(z.z, 0.0f, 1e-3f));

    // A zero-filled (FillBuffer-cleared / freshly reset) meta word must decode as
    // NO offset, not the max negative corner. Before the fix a raw 0 unpacked to
    // (-0.45,-0.45,-0.45) -- up to 7.2 m off in the largest cascade.
    Vec3 raw0 = RcgiUnpackOffset(0u);
    CHECK(Near(raw0.x, 0.0f, 1e-6f) && Near(raw0.y, 0.0f, 1e-6f) && Near(raw0.z, 0.0f, 1e-6f));
    // A packed zero offset is itself never the reserved sentinel word.
    CHECK(RcgiPackOffset(Vec3{0, 0, 0}) != 0u);
    // The all-min-negative corner (the only real offset that would pack to 0) is
    // nudged off the sentinel and still decodes near -kRcgiRelocMaxOffset.
    u32 corner = RcgiPackOffset(Vec3{-kRcgiRelocMaxOffset, -kRcgiRelocMaxOffset, -kRcgiRelocMaxOffset});
    CHECK(corner != 0u);
    Vec3 c = RcgiUnpackOffset(corner);
    const f32 step = 2.0f * kRcgiRelocMaxOffset / 1022.0f;
    CHECK(Near(c.x, -kRcgiRelocMaxOffset, step) && Near(c.y, -kRcgiRelocMaxOffset, step) &&
          Near(c.z, -kRcgiRelocMaxOffset, step));

    // A representative offset round-trips within the 10-bit quantization step
    // (2*0.45/1022 ~= 9e-4 per axis).
    const f32 q = 2.0f * kRcgiRelocMaxOffset / 1022.0f;
    Vec3 in{0.3f, -0.2f, 0.45f};
    Vec3 out = RcgiUnpackOffset(RcgiPackOffset(in));
    CHECK(Near(out.x, in.x, q) && Near(out.y, in.y, q) && Near(out.z, in.z, q));

    // Out-of-range input clamps to +-kRcgiRelocMaxOffset (never leaves the cell).
    Vec3 big = RcgiUnpackOffset(RcgiPackOffset(Vec3{5.0f, -5.0f, 0.0f}));
    CHECK(Near(big.x, kRcgiRelocMaxOffset, q));
    CHECK(Near(big.y, -kRcgiRelocMaxOffset, q));

    // Lanes are independent (packing one axis does not disturb the others).
    Vec3 only_y = RcgiUnpackOffset(RcgiPackOffset(Vec3{0.0f, 0.4f, 0.0f}));
    CHECK(Near(only_y.x, 0.0f, q) && Near(only_y.y, 0.4f, q) && Near(only_y.z, 0.0f, q));
  }

  if (g_failures == 0) std::printf("rcgi_interior_test: all checks passed\n");
  return g_failures == 0 ? 0 : 1;
}
