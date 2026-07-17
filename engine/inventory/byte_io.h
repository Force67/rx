#ifndef RX_INVENTORY_BYTE_IO_H_
#define RX_INVENTORY_BYTE_IO_H_

#include <cstring>
#include <vector>

#include "core/types.h"

// Minimal explicit-little-endian byte writer/reader shared by the serializers.
// Header-only, internal to the module (no export macros).

namespace rx::inventory::detail {

inline void PutU8(std::vector<u8>& b, u8 v) { b.push_back(v); }

inline void PutU16(std::vector<u8>& b, u16 v) {
  b.push_back(u8(v));
  b.push_back(u8(v >> 8));
}

inline void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}

inline void PutU64(std::vector<u8>& b, u64 v) {
  for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (8 * i)));
}

inline void PutF32(std::vector<u8>& b, f32 v) {
  u32 u;
  std::memcpy(&u, &v, 4);
  PutU32(b, u);
}

// Bounds-checked reader; sets ok=false and returns zeroes past the end so a
// truncated blob fails cleanly instead of reading out of bounds.
struct Reader {
  const u8* p;
  const u8* end;
  bool ok = true;

  explicit Reader(const std::vector<u8>& b) : p(b.data()), end(b.data() + b.size()) {}

  // Bytes not yet consumed. Lets callers bound a serialized count against the
  // input before reserving/allocating, and check for full consumption.
  size_t Remaining() const { return size_t(end - p); }

  u8 U8() {
    if (p + 1 > end) {
      ok = false;
      return 0;
    }
    return *p++;
  }
  u16 U16() {
    if (p + 2 > end) {
      ok = false;
      return 0;
    }
    u16 v = u16(p[0]) | u16(p[1]) << 8;
    p += 2;
    return v;
  }
  u32 U32() {
    if (p + 4 > end) {
      ok = false;
      return 0;
    }
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= u32(p[i]) << (8 * i);
    p += 4;
    return v;
  }
  u64 U64() {
    if (p + 8 > end) {
      ok = false;
      return 0;
    }
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= u64(p[i]) << (8 * i);
    p += 8;
    return v;
  }
  f32 F32() {
    u32 u = U32();
    f32 v;
    std::memcpy(&v, &u, 4);
    return v;
  }
};

}  // namespace rx::inventory::detail

#endif  // RX_INVENTORY_BYTE_IO_H_
