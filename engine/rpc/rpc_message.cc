#include "rpc/rpc_message.h"

#include <cstring>

namespace rx::rpc {
namespace {

// Wire magic 'RPC1' (FourCc is little-endian, so on disk this reads R, P, C, 1).
constexpr u32 kMagic = FourCc('R', 'P', 'C', '1');

// Structural limits. A name longer than this, too many args, or a string/blob
// larger than the cap is rejected outright rather than allocated, so a hostile
// header cannot drive a huge allocation or a long decode.
constexpr size_t kMaxName = 256;
constexpr size_t kMaxArgs = 1024;
constexpr size_t kMaxBytes = 16u * 1024 * 1024;  // 16 MiB per string/blob

void PutU16(std::vector<u8>& b, u16 v) {
  b.push_back(u8(v));
  b.push_back(u8(v >> 8));
}

void PutU32(std::vector<u8>& b, u32 v) {
  for (int i = 0; i < 4; ++i) b.push_back(u8(v >> (8 * i)));
}

void PutU64(std::vector<u8>& b, u64 v) {
  for (int i = 0; i < 8; ++i) b.push_back(u8(v >> (8 * i)));
}

void PutBytes(std::vector<u8>& b, const u8* p, size_t n) {
  b.insert(b.end(), p, p + n);
}

// Bounds-checked, little-endian cursor over the incoming buffer. Mirrors the
// pex.cc Reader pattern: any read past the end flips ok() and returns zero, so
// the decoder runs straight through and checks ok() once at the end. Read sizes
// derived from the buffer (lengths) are validated against the remaining bytes
// before they are used to advance, so a bogus length can never read out of range.
struct Reader {
  const u8* data;
  size_t size;
  size_t pos = 0;
  bool ok = true;

  bool Need(size_t n) {
    if (!ok || n > size - pos) {
      ok = false;
      return false;
    }
    return true;
  }

  u8 U8() {
    if (!Need(1)) return 0;
    return data[pos++];
  }
  u16 U16() {
    if (!Need(2)) return 0;
    const u8* p = data + pos;
    pos += 2;
    return u16(p[0]) | u16(p[1]) << 8;
  }
  u32 U32() {
    if (!Need(4)) return 0;
    const u8* p = data + pos;
    pos += 4;
    return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
  }
  u64 U64() {
    u64 lo = U32(), hi = U32();
    return lo | hi << 32;
  }
  i64 I64() {
    u64 v = U64();
    i64 r;
    std::memcpy(&r, &v, 8);
    return r;
  }
  f64 F64() {
    u64 v = U64();
    f64 r;
    std::memcpy(&r, &v, 8);
    return r;
  }
};

}  // namespace

std::vector<u8> EncodeCall(const RpcCall& call) {
  std::vector<u8> b;
  PutU32(b, kMagic);
  PutU16(b, static_cast<u16>(call.name.size()));
  PutBytes(b, reinterpret_cast<const u8*>(call.name.data()), call.name.size());
  PutU16(b, static_cast<u16>(call.args.size()));
  for (const RpcValue& a : call.args) {
    b.push_back(static_cast<u8>(a.type()));
    switch (a.type()) {
      case RpcValue::Type::kNull:
        break;
      case RpcValue::Type::kBool:
        b.push_back(a.as_bool() ? 1 : 0);
        break;
      case RpcValue::Type::kInt: {
        u64 v;
        i64 s = a.as_int();
        std::memcpy(&v, &s, 8);
        PutU64(b, v);
        break;
      }
      case RpcValue::Type::kFloat: {
        u64 v;
        f64 s = a.as_float();
        std::memcpy(&v, &s, 8);
        PutU64(b, v);
        break;
      }
      case RpcValue::Type::kString: {
        const std::string& s = a.as_string();
        PutU32(b, static_cast<u32>(s.size()));
        PutBytes(b, reinterpret_cast<const u8*>(s.data()), s.size());
        break;
      }
      case RpcValue::Type::kBlob: {
        const std::vector<u8>& s = a.as_blob();
        PutU32(b, static_cast<u32>(s.size()));
        PutBytes(b, s.data(), s.size());
        break;
      }
    }
  }
  return b;
}

std::optional<RpcCall> DecodeCall(const u8* data, size_t size) {
  Reader r{data, size};
  if (r.U32() != kMagic) return std::nullopt;

  RpcCall call;
  u16 name_len = r.U16();
  if (name_len > kMaxName) return std::nullopt;
  if (!r.Need(name_len)) return std::nullopt;
  call.name.assign(reinterpret_cast<const char*>(data + r.pos), name_len);
  r.pos += name_len;

  u16 arg_count = r.U16();
  if (arg_count > kMaxArgs) return std::nullopt;
  call.args.reserve(arg_count);
  for (u16 i = 0; i < arg_count; ++i) {
    u8 tag = r.U8();
    switch (static_cast<RpcValue::Type>(tag)) {
      case RpcValue::Type::kNull:
        call.args.emplace_back();
        break;
      case RpcValue::Type::kBool:
        call.args.emplace_back(r.U8() != 0);
        break;
      case RpcValue::Type::kInt:
        call.args.emplace_back(r.I64());
        break;
      case RpcValue::Type::kFloat:
        call.args.emplace_back(r.F64());
        break;
      case RpcValue::Type::kString: {
        u32 len = r.U32();
        if (len > kMaxBytes || !r.Need(len)) return std::nullopt;
        std::string s(reinterpret_cast<const char*>(data + r.pos), len);
        r.pos += len;
        call.args.emplace_back(std::move(s));
        break;
      }
      case RpcValue::Type::kBlob: {
        u32 len = r.U32();
        if (len > kMaxBytes || !r.Need(len)) return std::nullopt;
        std::vector<u8> blob(data + r.pos, data + r.pos + len);
        r.pos += len;
        call.args.emplace_back(std::move(blob));
        break;
      }
      default:
        return std::nullopt;  // unknown type tag means the stream is corrupt
    }
    if (!r.ok) return std::nullopt;
  }

  if (!r.ok) return std::nullopt;
  if (r.pos != size) return std::nullopt;  // trailing garbage
  return call;
}

}  // namespace rx::rpc
