#ifndef RX_LOCOMOTION_INTERNAL_MATH_H_
#define RX_LOCOMOTION_INTERNAL_MATH_H_

// Module-internal scalar/vector helpers shared by the locomotion .cc files
// (controller / estimator / gait / footstep / whole_body). Header-only and NOT
// part of the public rx::locomotion surface: nothing here is exported and no
// header outside the module includes it. Each helper is the single canonical
// copy of a routine that used to be re-implemented, with drifting names, in
// several files. Behaviour is bit-identical to those originals.

#include <cmath>

#include "core/math.h"
#include "core/types.h"

namespace rx::locomotion::internal {

// Finiteness predicates.
inline bool FiniteScalar(f32 x) { return std::isfinite(x); }
inline bool FiniteV(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
inline bool FiniteQ(const Quat& q) {
  return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

// Clamp a scalar to [lo, hi].
inline f32 Clampf(f32 x, f32 lo, f32 hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Linear interpolation of scalars.
inline f32 Lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// Drop the vertical component of a world vector.
inline Vec3 Planar(const Vec3& v) { return {v.x, 0.0f, v.z}; }

// Planar (x,z) magnitude of a world vector, ignoring the vertical component.
inline f32 PlanarLength(const Vec3& v) { return std::sqrt(v.x * v.x + v.z * v.z); }

// Clamp a vector's length to `max_len` (no-op below it; safe for the zero vec).
inline Vec3 ClampLength(const Vec3& v, f32 max_len) {
  const f32 len = Length(v);
  if (len <= max_len || len <= 0) return v;
  return v * (max_len / len);
}

// Exponential smoothing coefficient for a first-order lag (tau seconds) over dt.
// 0 for dt <= 0 or tau <= 0 (holds the current value); -> 1 for large dt.
inline f32 SmoothingAlpha(f32 dt, f32 tau) {
  if (!(dt > 0) || !(tau > 0)) return 0;
  return 1.0f - std::exp(-dt / tau);
}

}  // namespace rx::locomotion::internal

#endif  // RX_LOCOMOTION_INTERNAL_MATH_H_
