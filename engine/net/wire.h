#ifndef RX_NET_WIRE_H_
#define RX_NET_WIRE_H_

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"

namespace rx::net {

// Little-endian wire codec for the engine's net messages. The writer appends
// into a flat byte vector; the reader is bounds-checked and turns any overrun
// into ok() == false rather than UB, so a truncated or hostile packet decodes
// to "malformed" and the caller drops it.

class ByteWriter {
 public:
  void U8(u8 v) { buf_.push_back(v); }
  void U16(u16 v) {
    buf_.push_back(static_cast<u8>(v));
    buf_.push_back(static_cast<u8>(v >> 8));
  }
  void U32(u32 v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<u8>(v >> (8 * i)));
  }
  void U64(u64 v) {
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<u8>(v >> (8 * i)));
  }
  void F32(f32 v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    U32(bits);
  }
  void Bool(bool v) { U8(v ? 1 : 0); }
  // Length-prefixed (u16) string; longer input is truncated.
  void Str(std::string_view s) {
    const u16 len = static_cast<u16>(s.size() > 0xffff ? 0xffff : s.size());
    U16(len);
    buf_.insert(buf_.end(), s.data(), s.data() + len);
  }

  std::vector<u8> Take() { return std::move(buf_); }
  size_t size() const { return buf_.size(); }

 private:
  std::vector<u8> buf_;
};

class ByteReader {
 public:
  ByteReader(const u8* data, size_t size) : data_(data), size_(size) {}

  u8 U8() {
    if (!Want(1)) return 0;
    return data_[pos_++];
  }
  u16 U16() {
    if (!Want(2)) return 0;
    u16 v = static_cast<u16>(data_[pos_]) | static_cast<u16>(data_[pos_ + 1]) << 8;
    pos_ += 2;
    return v;
  }
  u32 U32() {
    if (!Want(4)) return 0;
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<u32>(data_[pos_ + i]) << (8 * i);
    pos_ += 4;
    return v;
  }
  u64 U64() {
    if (!Want(8)) return 0;
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<u64>(data_[pos_ + i]) << (8 * i);
    pos_ += 8;
    return v;
  }
  f32 F32() {
    const u32 bits = U32();
    f32 v;
    std::memcpy(&v, &bits, 4);
    return v;
  }
  bool Bool() { return U8() != 0; }
  std::string Str() {
    const u16 len = U16();
    if (!Want(len)) return {};
    std::string s(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return s;
  }

  bool ok() const { return ok_; }
  size_t remaining() const { return ok_ ? size_ - pos_ : 0; }

 private:
  bool Want(size_t n) {
    if (!ok_ || size_ - pos_ < n) {
      ok_ = false;
      return false;
    }
    return true;
  }

  const u8* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
  bool ok_ = true;
};

}  // namespace rx::net

#endif  // RX_NET_WIRE_H_
