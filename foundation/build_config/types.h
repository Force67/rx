#ifndef RX_FOUNDATION_BUILD_CONFIG_TYPES_H_
#define RX_FOUNDATION_BUILD_CONFIG_TYPES_H_

#include "base/arch.h"
#include "base/containers/span.h"
#include "base/strings/string_ref.h"

namespace rx {

// base's fixed-width types. u64 is unsigned long long there, not the
// unsigned long uint64_t is on LP64: same width, but a distinct type for
// overloads, pointer parameters and printf, so C APIs taking uint64_t* need
// their own variable.
using arch_types::f32;
using arch_types::f64;
using arch_types::i16;
using arch_types::i32;
using arch_types::i64;
using arch_types::i8;
using arch_types::u16;
using arch_types::u32;
using arch_types::u64;
using arch_types::u8;

using ByteSpan = base::Span<const u8>;

inline u64 Fnv1a(base::StringRef str) {
  u64 hash = 0xcbf29ce484222325ull;
  for (char c : str) {
    hash ^= static_cast<u8>(c);
    hash *= 0x100000001b3ull;
  }
  return hash;
}

constexpr u32 FourCc(char a, char b, char c, char d) {
  return static_cast<u32>(a) | static_cast<u32>(b) << 8 | static_cast<u32>(c) << 16 |
         static_cast<u32>(d) << 24;
}

}  // namespace rx

#endif  // RX_FOUNDATION_BUILD_CONFIG_TYPES_H_
