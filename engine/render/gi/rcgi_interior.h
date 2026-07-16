#ifndef RX_RENDER_RCGI_INTERIOR_H_
#define RX_RENDER_RCGI_INTERIOR_H_

#include <cmath>

#include "core/math.h"

// Pure-logic helpers for RCGI leak & occlusion hardening (Phase 3):
//   - interior-volume classification (point-in-box), the CPU mirror of the
//     GPU `RcgiPointInInterior` used to zero cross-class probe blends.
//   - probe-relocation offset packing, the CPU mirror of the shader
//     `RcgiPackOffset`/`RcgiUnpackOffset` so the packing round-trip and the
//     classification predicate are unit-testable off-GPU (rcgi_interior_test).
//
// These have no GPU dependency on purpose: they are shared by the renderer
// wiring (volume upload / count) and by the headless test.
namespace rx::render {

// An axis-aligned interior volume in world space. recreation forwards interior
// cell bounds (later Skyrim room-bound XRGN); rx games (mayorhem-style) forward
// their building interiors. Kept axis-aligned: cheap point tests, and the leak
// it fixes (outdoor probe bleeding through a doorway) does not need oriented
// boxes to resolve.
struct InteriorVolume {
  Vec3 min;
  Vec3 max;
};

// Hard cap on volumes uploaded to the GPU (one small UBO). AC Shadows classifies
// against a handful of interiors near the camera; 64 is generous headroom.
inline constexpr u32 kMaxInteriorVolumes = 64;

// Relocation offset is bounded to +-kRcgiRelocMaxOffset of the cascade spacing so
// a probe never leaves its own cell (matches AC Shadows' +-0.45-cell clamp).
inline constexpr f32 kRcgiRelocMaxOffset = 0.45f;

// True when `p` lies inside any of the first `count` volumes. Must stay
// bit-identical in intent to the shader `RcgiPointInInterior`.
inline bool PointInInteriorVolumes(const InteriorVolume* volumes, u32 count, const Vec3& p) {
  for (u32 i = 0; i < count; ++i) {
    const InteriorVolume& v = volumes[i];
    if (p.x >= v.min.x && p.y >= v.min.y && p.z >= v.min.z && p.x <= v.max.x && p.y <= v.max.y &&
        p.z <= v.max.z) {
      return true;
    }
  }
  return false;
}

// Pack a per-axis relocation offset expressed as a *fraction of cascade spacing*
// (each component clamped to +-kRcgiRelocMaxOffset) into a single u32: three
// 10-bit signed-normalized lanes. CPU mirror of the shader `RcgiPackOffset`.
inline u32 RcgiPackOffset(const Vec3& frac) {
  auto lane = [](f32 f) -> u32 {
    f32 n = f / kRcgiRelocMaxOffset;
    n = n < -1.0f ? -1.0f : (n > 1.0f ? 1.0f : n);
    i32 q = static_cast<i32>(std::lround(n * 511.0f)) + 512;  // [1, 1023] around 512
    if (q < 0) q = 0;
    if (q > 1023) q = 1023;
    return static_cast<u32>(q);
  };
  u32 packed = lane(frac.x) | (lane(frac.y) << 10) | (lane(frac.z) << 20);
  // Raw word 0 is reserved as the "no offset" sentinel so a zero-cleared meta
  // buffer reads as unrelocated, not the max negative corner (mirror of the
  // shader RcgiPackOffset). Nudge the all-min-negative offset off the sentinel.
  return packed == 0u ? 1u : packed;
}

// Inverse of RcgiPackOffset: returns the per-axis fraction of spacing. Raw word 0
// decodes as no offset (the zero-cleared-buffer default), mirroring the shader.
inline Vec3 RcgiUnpackOffset(u32 p) {
  if (p == 0u) return Vec3{0.0f, 0.0f, 0.0f};
  auto lane = [](u32 q) -> f32 {
    f32 n = (static_cast<f32>(q & 1023u) - 512.0f) / 511.0f;
    n = n < -1.0f ? -1.0f : (n > 1.0f ? 1.0f : n);
    return n * kRcgiRelocMaxOffset;
  };
  return Vec3{lane(p), lane(p >> 10), lane(p >> 20)};
}

}  // namespace rx::render

#endif  // RX_RENDER_RCGI_INTERIOR_H_
