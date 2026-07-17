#ifndef RX_PLACEMENT_PLACEMENT_MATH_H_
#define RX_PLACEMENT_PLACEMENT_MATH_H_

#include <cmath>

#include "core/math.h"
#include "core/types.h"

// Deterministic helpers shared between the CPU reference path and the GPU
// pipeline. Every function here mirrors placement_common.hlsli op for op:
// integer math is bit-exact across the two, float math agrees to rounding.
// Placement identity is positional - hash(seed, tile, pattern point, layer) -
// so a regenerated tile reproduces the same instances regardless of the order
// the GPU appended them in.

namespace rx::placement {

inline u32 PcgHash(u32 v) {
  u32 state = v * 747796405u + 2891336453u;
  u32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

inline u32 HashCombine(u32 a, u32 b) { return PcgHash(a ^ (b * 0x9E3779B9u)); }

// [0,1) from the top 24 bits, matching the HLSL conversion exactly.
inline f32 HashToUnit(u32 h) { return static_cast<f32>(h >> 8u) * (1.0f / 16777216.0f); }

// Stable identity of one placed object: pattern-point rank + tile coordinate
// + layer, independent of GPU append order.
inline u32 InstanceSeed(u32 world_seed, i32 tile_x, i32 tile_z, u32 point, u32 layer) {
  u32 h = HashCombine(world_seed, static_cast<u32>(tile_x) * 0x8DA6B343u);
  h = HashCombine(h, static_cast<u32>(tile_z) * 0xD8163841u);
  h = HashCombine(h, point * 0xCB1AB31Fu);
  return HashCombine(h, layer + 1u);
}

inline f32 Smoothstep01(f32 x) {
  x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
  return x * x * (3.0f - 2.0f * x);
}

// Value noise on an integer lattice with smoothstep interpolation. `x`/`z` in
// meters, `feature_size` is the lattice spacing.
inline f32 ValueNoise(f32 x, f32 z, f32 feature_size, u32 seed) {
  if (feature_size <= 0.0f) return 0.5f;
  f32 fx = x / feature_size;
  f32 fz = z / feature_size;
  f32 ix = std::floor(fx);
  f32 iz = std::floor(fz);
  f32 tx = Smoothstep01(fx - ix);
  f32 tz = Smoothstep01(fz - iz);
  i32 cx = static_cast<i32>(ix);
  i32 cz = static_cast<i32>(iz);
  auto corner = [seed](i32 lx, i32 lz) {
    u32 h = HashCombine(seed, static_cast<u32>(lx) * 0x8DA6B343u);
    return HashToUnit(HashCombine(h, static_cast<u32>(lz) * 0xD8163841u));
  };
  f32 v00 = corner(cx, cz);
  f32 v10 = corner(cx + 1, cz);
  f32 v01 = corner(cx, cz + 1);
  f32 v11 = corner(cx + 1, cz + 1);
  f32 a = v00 + (v10 - v00) * tx;
  f32 b = v01 + (v11 - v01) * tx;
  return a + (b - a) * tz;
}

struct OrientedPoint {
  Vec3 position;
  Vec3 normal;
};

// Final transform of the PLACEMENT stage: uniform scale, yaw around the
// blended up axis, translation. `tilt` blends the object's up vector from
// world up toward the surface normal.
inline Mat4 BuildPlacementTransform(const OrientedPoint& point, f32 yaw, f32 scale,
                                    f32 tilt, f32 y_offset) {
  Vec3 up{0.0f, 1.0f, 0.0f};
  Vec3 axis{up.x + (point.normal.x - up.x) * tilt, up.y + (point.normal.y - up.y) * tilt,
            up.z + (point.normal.z - up.z) * tilt};
  f32 len = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
  if (len < 1e-5f) {
    axis = up;
  } else {
    axis = {axis.x / len, axis.y / len, axis.z / len};
  }
  // Orthonormal basis around `axis`, yawed by `yaw`.
  f32 c = std::cos(yaw);
  f32 s = std::sin(yaw);
  Vec3 ref = std::fabs(axis.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
  Vec3 tangent{ref.y * axis.z - ref.z * axis.y, ref.z * axis.x - ref.x * axis.z,
               ref.x * axis.y - ref.y * axis.x};
  f32 tlen = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
  tangent = {tangent.x / tlen, tangent.y / tlen, tangent.z / tlen};
  // tangent x axis, so [x=tangent, y=axis, z=bitangent] is right-handed
  // (instance groups reject mirrored transforms).
  Vec3 bitangent{tangent.y * axis.z - tangent.z * axis.y,
                 tangent.z * axis.x - tangent.x * axis.z,
                 tangent.x * axis.y - tangent.y * axis.x};
  Vec3 basis_x{tangent.x * c + bitangent.x * s, tangent.y * c + bitangent.y * s,
               tangent.z * c + bitangent.z * s};
  Vec3 basis_z{-tangent.x * s + bitangent.x * c, -tangent.y * s + bitangent.y * c,
               -tangent.z * s + bitangent.z * c};

  Mat4 m = Mat4::Identity();
  m.m[0] = basis_x.x * scale;
  m.m[1] = basis_x.y * scale;
  m.m[2] = basis_x.z * scale;
  m.m[4] = axis.x * scale;
  m.m[5] = axis.y * scale;
  m.m[6] = axis.z * scale;
  m.m[8] = basis_z.x * scale;
  m.m[9] = basis_z.y * scale;
  m.m[10] = basis_z.z * scale;
  m.m[12] = point.position.x;
  m.m[13] = point.position.y + y_offset;
  m.m[14] = point.position.z;
  return m;
}

}  // namespace rx::placement

#endif  // RX_PLACEMENT_PLACEMENT_MATH_H_
