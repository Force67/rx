#ifndef RX_RENDER2D_TYPES2D_H_
#define RX_RENDER2D_TYPES2D_H_

#include "core/math.h"
#include "core/types.h"

// Small value types shared across the 2D renderer. Kept header-only and free of
// any GPU / RHI type so gameplay code and unit tests can use them without a
// device. rx's core math has no 2D vector, so the module carries its own.
namespace rx::render2d {

struct Vec2 {
  f32 x = 0, y = 0;

  Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  Vec2 operator*(f32 s) const { return {x * s, y * s}; }
  Vec2& operator+=(Vec2 o) {
    x += o.x;
    y += o.y;
    return *this;
  }
};

inline f32 Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
inline f32 Length(Vec2 v) { return std::sqrt(Dot(v, v)); }
inline Vec2 Lerp(Vec2 a, Vec2 b, f32 t) { return a + (b - a) * t; }
inline Vec2 Normalize(Vec2 v) {
  f32 l = Length(v);
  return l > 0 ? Vec2{v.x / l, v.y / l} : v;
}

// Integer grid coordinate (tile indices, cell picks).
struct Vec2i {
  i32 x = 0, y = 0;
  bool operator==(const Vec2i& o) const { return x == o.x && y == o.y; }
  bool operator!=(const Vec2i& o) const { return !(*this == o); }
};

// A colour in linear-ish [0,1] rgba. The sprite pipeline multiplies it into the
// sampled texel; lights accumulate in the same space. Authoring is sRGB-ish
// (see FromSrgb) so hand-picked palettes read as expected on screen.
struct Color {
  f32 r = 1, g = 1, b = 1, a = 1;

  static constexpr Color White() { return {1, 1, 1, 1}; }
  static constexpr Color Black() { return {0, 0, 0, 1}; }
  static constexpr Color Clear() { return {0, 0, 0, 0}; }

  // 0xRRGGBB / 0xAARRGGBB packed literals, the way palettes are usually written.
  static Color Rgb(u32 rgb) {
    return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f,
            1.0f};
  }
  static Color Rgba(u32 argb) {
    return {((argb >> 16) & 0xff) / 255.0f, ((argb >> 8) & 0xff) / 255.0f, (argb & 0xff) / 255.0f,
            ((argb >> 24) & 0xff) / 255.0f};
  }

  Color WithAlpha(f32 alpha) const { return {r, g, b, alpha}; }
  Color Scaled(f32 s) const { return {r * s, g * s, b * s, a}; }
};

// A rectangle in some 2D space (world units or texel uv). Origin is the min
// corner; +x is right and +y is down, matching tile / screen conventions.
struct Rect {
  f32 x = 0, y = 0, w = 0, h = 0;

  f32 left() const { return x; }
  f32 top() const { return y; }
  f32 right() const { return x + w; }
  f32 bottom() const { return y + h; }
  Vec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }

  bool Contains(Vec2 p) const {
    return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
  }
  bool Intersects(const Rect& o) const {
    return x < o.x + o.w && x + w > o.x && y < o.y + o.h && y + h > o.y;
  }
};

}  // namespace rx::render2d

#endif  // RX_RENDER2D_TYPES2D_H_
