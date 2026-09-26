#ifndef RX_CORE_SCALAR_H_
#define RX_CORE_SCALAR_H_

#include <initializer_list>

namespace rx {

// Min/Max/Clamp/Lerp with the exact comparison order of std::min, std::max,
// std::clamp and std::lerp, which is what this engine was tuned and captured
// against. base::Min/Max/Clamp compare the other way round, so they disagree
// on NaN (base::Clamp maps NaN to hi, std::clamp passes it through) and on
// which zero wins between -0 and +0. Swapping one for the other is not a
// no-op for the picture or for the physics NaN guards.

// std::min: the second argument only when it is strictly smaller.
template <typename T>
constexpr T Min(T a, T b) {
  return b < a ? b : a;
}

// std::max: the second argument only when the first is strictly smaller.
template <typename T>
constexpr T Max(T a, T b) {
  return a < b ? b : a;
}

// std::min({...}): the first of the smallest.
template <typename T>
constexpr T Min(std::initializer_list<T> values) {
  const T* it = values.begin();
  T result = *it;
  for (++it; it != values.end(); ++it) {
    if (*it < result) result = *it;
  }
  return result;
}

// std::max({...}): the first of the largest.
template <typename T>
constexpr T Max(std::initializer_list<T> values) {
  const T* it = values.begin();
  T result = *it;
  for (++it; it != values.end(); ++it) {
    if (result < *it) result = *it;
  }
  return result;
}

// std::clamp as libstdc++ spells it: NaN in, NaN out, and hi wins when a
// caller passes lo > hi (an empty range clamped to count - 1, say).
template <typename T>
constexpr T Clamp(T v, T lo, T hi) {
  return Min(Max(v, lo), hi);
}

// std::lerp for floating point: exact at both ends, monotonic in t, and
// bounded by b past t == 1, where the naive a + t * (b - a) is none of those.
template <typename T>
constexpr T Lerp(T a, T b, T t) {
  if ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) return t * b + (1 - t) * a;
  if (t == 1) return b;
  const T x = a + t * (b - a);
  return (t > 1) == (b > a) ? (b < x ? x : b) : (x < b ? x : b);
}

// std::bit_cast, on the builtin every supported compiler has.
template <typename To, typename From>
constexpr To BitCast(const From& from) {
  static_assert(sizeof(To) == sizeof(From), "BitCast needs equal sizes");
  return __builtin_bit_cast(To, from);
}

}  // namespace rx

#endif  // RX_CORE_SCALAR_H_
