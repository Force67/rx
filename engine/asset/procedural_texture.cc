#include "asset/procedural_texture.h"

#include <algorithm>
#include <cmath>

#include "core/math.h"

namespace rx::asset {
namespace {

f32 Fract(f32 x) { return x - std::floor(x); }

f32 Smoothstep(f32 edge0, f32 edge1, f32 x) {
  if (edge1 <= edge0) return x < edge0 ? 0.0f : 1.0f;
  f32 t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

// Half-width of every pattern edge, in cell fractions. Wide enough that the
// derived normal map gets a few texels of slope at the default 256px, narrow
// enough that a checker still reads as a checker.
constexpr f32 kEdgeSoftness = 0.02f;

// 0 at the cell boundary, 1 at the cell centre.
f32 EdgeDistance(f32 t) {
  f32 f = Fract(t);
  return std::min(f, 1.0f - f) * 2.0f;
}

// A square wave alternating 0 and 1 per unit cell of `t`, crossfaded across the
// cell boundary rather than stepped. Both cells reach exactly 0.5 at the
// boundary, which is what keeps it continuous (and the checker antialiased).
f32 SoftSquare(f32 t) {
  f32 cell = std::floor(t);
  f32 parity = std::fmod(std::abs(cell), 2.0f) >= 1.0f ? 1.0f : 0.0f;
  f32 inside = 0.5f + 0.5f * Smoothstep(0.0f, kEdgeSoftness * 2.0f, EdgeDistance(t));
  return parity * inside + (1.0f - parity) * (1.0f - inside);
}

u32 HashLattice(i32 x, i32 y, u32 seed) {
  u32 h = seed * 374761393u + static_cast<u32>(x) * 668265263u +
          static_cast<u32>(y) * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

// Value noise on a `period` x `period` lattice that wraps with it, so the field
// tiles with the uv square (a sphere's uv seam would show any that did not).
f32 ValueNoise(f32 x, f32 y, i32 period, u32 seed) {
  if (period < 1) period = 1;
  i32 x0 = static_cast<i32>(std::floor(x)), y0 = static_cast<i32>(std::floor(y));
  f32 fx = x - static_cast<f32>(x0), fy = y - static_cast<f32>(y0);
  auto wrap = [period](i32 v) { return ((v % period) + period) % period; };
  auto corner = [&](i32 cx, i32 cy) {
    return static_cast<f32>(HashLattice(wrap(cx), wrap(cy), seed)) * (1.0f / 4294967296.0f);
  };
  f32 sx = fx * fx * (3.0f - 2.0f * fx), sy = fy * fy * (3.0f - 2.0f * fy);
  f32 top = corner(x0, y0) + (corner(x0 + 1, y0) - corner(x0, y0)) * sx;
  f32 bottom = corner(x0, y0 + 1) + (corner(x0 + 1, y0 + 1) - corner(x0, y0 + 1)) * sx;
  return top + (bottom - top) * sy;
}

// Encodes a linear channel with the sRGB transfer function, for the textures
// bound to a slot the GPU samples as sRGB.
u8 EncodeSrgb(f32 linear) {
  f32 v = std::clamp(linear, 0.0f, 1.0f);
  f32 encoded = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
  return static_cast<u8>(std::lround(encoded * 255.0f));
}

u8 EncodeLinear(f32 v) {
  return static_cast<u8>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

Texture BeginTexture(const PatternDesc& desc, bool srgb, AssetId id) {
  Texture texture;
  texture.id = id;
  texture.format = TextureFormat::kRgba8;
  texture.width = std::max(desc.width, 1u);
  texture.height = std::max(desc.height, 1u);
  texture.is_srgb = srgb;
  texture.data.resize(static_cast<size_t>(texture.width) * texture.height * 4);
  return texture;
}

}  // namespace

bool ParsePatternKind(std::string_view name, PatternKind* out) {
  if (name == "checker") *out = PatternKind::kChecker;
  else if (name == "grid") *out = PatternKind::kGrid;
  else if (name == "brick") *out = PatternKind::kBrick;
  else if (name == "gradient") *out = PatternKind::kGradient;
  else if (name == "noise") *out = PatternKind::kNoise;
  else return false;
  return true;
}

f32 SamplePattern(const PatternDesc& desc, f32 u, f32 v) {
  const f32 scale = std::max(desc.scale, 0.0001f);
  const f32 line = std::clamp(desc.line_width, 0.0f, 0.9f);
  switch (desc.kind) {
    case PatternKind::kChecker: {
      f32 a = SoftSquare(u * scale), b = SoftSquare(v * scale);
      return a + b - 2.0f * a * b;  // soft xor
    }
    case PatternKind::kGrid: {
      f32 d = std::min(EdgeDistance(u * scale), EdgeDistance(v * scale));
      return Smoothstep(line - kEdgeSoftness, line + kEdgeSoftness, d);
    }
    case PatternKind::kBrick: {
      // `scale` counts courses; bricks are twice as wide as they are tall and
      // every other course is offset by half a brick.
      f32 course = std::floor(v * scale);
      f32 across = u * scale * 0.5f + (std::fmod(std::abs(course), 2.0f) >= 1.0f ? 0.5f : 0.0f);
      // The joint has to be the same uv width both ways, and a brick is twice
      // as wide as a course is tall, so the horizontal fraction is halved.
      f32 dv = Smoothstep(line - kEdgeSoftness, line + kEdgeSoftness, EdgeDistance(v * scale));
      f32 du = Smoothstep(line * 0.5f - kEdgeSoftness, line * 0.5f + kEdgeSoftness,
                          EdgeDistance(across));
      return du * dv;
    }
    case PatternKind::kGradient:
      return std::clamp(v, 0.0f, 1.0f);
    case PatternKind::kNoise: {
      f32 sum = 0.0f, amplitude = 1.0f, total = 0.0f;
      i32 period = std::max(1, static_cast<i32>(std::lround(scale)));
      for (int octave = 0; octave < 4; ++octave) {
        sum += ValueNoise(u * static_cast<f32>(period), v * static_cast<f32>(period), period,
                          desc.seed + static_cast<u32>(octave) * 7919u) *
               amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        period *= 2;
      }
      return sum / total;
    }
  }
  return 0.0f;
}

Texture MakePatternTexture(const PatternDesc& desc, const f32 color_a[3], const f32 color_b[3],
                           bool srgb, AssetId id) {
  Texture texture = BeginTexture(desc, srgb, id);
  for (u32 y = 0; y < texture.height; ++y) {
    for (u32 x = 0; x < texture.width; ++x) {
      f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(texture.width);
      f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(texture.height);
      f32 mask = SamplePattern(desc, u, v);
      u8* texel = &texture.data[(static_cast<size_t>(y) * texture.width + x) * 4];
      for (int c = 0; c < 3; ++c) {
        f32 value = color_a[c] + (color_b[c] - color_a[c]) * mask;
        texel[c] = srgb ? EncodeSrgb(value) : EncodeLinear(value);
      }
      texel[3] = 255;
    }
  }
  return texture;
}

Texture MakePatternNormalMap(const PatternDesc& desc, f32 relief, AssetId id) {
  Texture texture = BeginTexture(desc, /*srgb=*/false, id);
  const f32 du = 1.0f / static_cast<f32>(texture.width);
  const f32 dv = 1.0f / static_cast<f32>(texture.height);
  for (u32 y = 0; y < texture.height; ++y) {
    for (u32 x = 0; x < texture.width; ++x) {
      f32 u = (static_cast<f32>(x) + 0.5f) * du;
      f32 v = (static_cast<f32>(y) + 0.5f) * dv;
      // Central differences in uv units, so the slope is a property of the
      // pattern and the relief depth rather than of the map's resolution.
      f32 dhdu = (SamplePattern(desc, u + du, v) - SamplePattern(desc, u - du, v)) / (2.0f * du);
      f32 dhdv = (SamplePattern(desc, u, v + dv) - SamplePattern(desc, u, v - dv)) / (2.0f * dv);
      Vec3 n = Normalize(Vec3{-dhdu * relief, -dhdv * relief, 1.0f});
      u8* texel = &texture.data[(static_cast<size_t>(y) * texture.width + x) * 4];
      texel[0] = EncodeLinear(n.x * 0.5f + 0.5f);
      texel[1] = EncodeLinear(n.y * 0.5f + 0.5f);
      texel[2] = EncodeLinear(n.z * 0.5f + 0.5f);
      texel[3] = 255;
    }
  }
  return texture;
}

Texture MakePatternRoughnessMap(const PatternDesc& desc, f32 roughness_a, f32 roughness_b,
                                AssetId id) {
  Texture texture = BeginTexture(desc, /*srgb=*/false, id);
  for (u32 y = 0; y < texture.height; ++y) {
    for (u32 x = 0; x < texture.width; ++x) {
      f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(texture.width);
      f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(texture.height);
      f32 mask = SamplePattern(desc, u, v);
      u8* texel = &texture.data[(static_cast<size_t>(y) * texture.width + x) * 4];
      texel[0] = 255;
      texel[1] = EncodeLinear(roughness_a + (roughness_b - roughness_a) * mask);
      texel[2] = 255;
      texel[3] = 255;
    }
  }
  return texture;
}

}  // namespace rx::asset
