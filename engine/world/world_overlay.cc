#include "world/world_overlay.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace rx::world {
namespace {

constexpr u8 kMagic[8] = {'R', 'X', 'O', 'V', 'R', 'L', 'A', 'Y'};
constexpr u32 kVersion = 1;
constexpr u32 kMaximumDestroyed = 16'000'000;
constexpr u32 kMaximumMoves = 16'000'000;
constexpr u32 kDestroyedBytes = 8;
constexpr u32 kMoveBytes = 40;

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

u64 Checksum(std::span<const u8> bytes) {
  u64 hash = 0xcbf29ce484222325ull;
  for (u8 byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

void AppendU32(base::Vector<u8>* bytes, u32 value) {
  for (u32 shift = 0; shift < 32; shift += 8) bytes->push_back(static_cast<u8>(value >> shift));
}

void AppendU64(base::Vector<u8>* bytes, u64 value) {
  for (u32 shift = 0; shift < 64; shift += 8) bytes->push_back(static_cast<u8>(value >> shift));
}

void AppendF32(base::Vector<u8>* bytes, f32 value) { AppendU32(bytes, std::bit_cast<u32>(value)); }

class Cursor {
 public:
  explicit Cursor(std::span<const u8> bytes) : bytes_(bytes) {}

  bool ok() const { return ok_; }
  size_t offset() const { return offset_; }

  bool Take(size_t count, const u8** out) {
    if (!ok_ || bytes_.size() - offset_ < count) {
      ok_ = false;
      return false;
    }
    *out = bytes_.data() + offset_;
    offset_ += count;
    return true;
  }

  u8 U8() {
    const u8* data = nullptr;
    if (!Take(1, &data)) return 0;
    return *data;
  }
  u32 U32() {
    u32 value = 0;
    for (u32 shift = 0; shift < 32; shift += 8) value |= static_cast<u32>(U8()) << shift;
    return value;
  }
  u64 U64() {
    u64 value = 0;
    for (u32 shift = 0; shift < 64; shift += 8) value |= static_cast<u64>(U8()) << shift;
    return value;
  }
  f32 F32() { return std::bit_cast<f32>(U32()); }

 private:
  std::span<const u8> bytes_;
  size_t offset_ = 0;
  bool ok_ = true;
};

bool Finite(const OverlayMove& move) {
  return std::isfinite(move.position.x) && std::isfinite(move.position.y) &&
         std::isfinite(move.position.z) && std::isfinite(move.rotation.x) &&
         std::isfinite(move.rotation.y) && std::isfinite(move.rotation.z) &&
         std::isfinite(move.rotation.w) && std::isfinite(move.scale);
}

}  // namespace

void WorldOverlay::Destroy(u64 stable_id) {
  auto it = std::lower_bound(destroyed_.begin(), destroyed_.end(), stable_id);
  if (it == destroyed_.end() || *it != stable_id) destroyed_.insert(it, stable_id);
  auto move = std::lower_bound(
      moves_.begin(), moves_.end(), stable_id,
      [](const OverlayMove& entry, u64 wanted) { return entry.stable_id < wanted; });
  if (move != moves_.end() && move->stable_id == stable_id) moves_.erase(move);
}

bool WorldOverlay::IsDestroyed(u64 stable_id) const {
  return std::binary_search(destroyed_.begin(), destroyed_.end(), stable_id);
}

void WorldOverlay::Move(u64 stable_id, Vec3 position, Quat rotation, f32 scale) {
  if (IsDestroyed(stable_id)) return;
  auto it = std::lower_bound(
      moves_.begin(), moves_.end(), stable_id,
      [](const OverlayMove& entry, u64 wanted) { return entry.stable_id < wanted; });
  if (it != moves_.end() && it->stable_id == stable_id) {
    it->position = position;
    it->rotation = rotation;
    it->scale = scale;
    return;
  }
  moves_.insert(it, OverlayMove{stable_id, position, rotation, scale});
}

const OverlayMove* WorldOverlay::FindMove(u64 stable_id) const {
  auto it = std::lower_bound(
      moves_.begin(), moves_.end(), stable_id,
      [](const OverlayMove& entry, u64 wanted) { return entry.stable_id < wanted; });
  return it != moves_.end() && it->stable_id == stable_id ? it : nullptr;
}

void WorldOverlay::Forget(u64 stable_id) {
  auto destroyed = std::lower_bound(destroyed_.begin(), destroyed_.end(), stable_id);
  if (destroyed != destroyed_.end() && *destroyed == stable_id) destroyed_.erase(destroyed);
  auto move = std::lower_bound(
      moves_.begin(), moves_.end(), stable_id,
      [](const OverlayMove& entry, u64 wanted) { return entry.stable_id < wanted; });
  if (move != moves_.end() && move->stable_id == stable_id) moves_.erase(move);
}

void WorldOverlay::Clear() {
  destroyed_.clear();
  moves_.clear();
}

bool WorldOverlay::TouchesRange(u64 first, u32 count) const {
  if (count == 0) return false;
  // Saturating rather than wrapping: the index refuses a range that runs off the
  // end of the id space, but this is public and does not get to assume it.
  const u64 last = first > ~u64{0} - (count - 1) ? ~u64{0} : first + count - 1;
  auto destroyed = std::lower_bound(destroyed_.begin(), destroyed_.end(), first);
  if (destroyed != destroyed_.end() && *destroyed <= last) return true;
  auto move = std::lower_bound(
      moves_.begin(), moves_.end(), first,
      [](const OverlayMove& entry, u64 wanted) { return entry.stable_id < wanted; });
  return move != moves_.end() && move->stable_id <= last;
}

bool WorldOverlay::Encode(base::Vector<u8>* out, std::string* error) const {
  if (!out) return false;
  if (destroyed_.size() > kMaximumDestroyed || moves_.size() > kMaximumMoves) {
    SetError(error, "world overlay: too many deltas");
    return false;
  }
  base::Vector<u8> body;
  for (u64 id : destroyed_) AppendU64(&body, id);
  for (const OverlayMove& move : moves_) {
    AppendU64(&body, move.stable_id);
    AppendF32(&body, move.position.x);
    AppendF32(&body, move.position.y);
    AppendF32(&body, move.position.z);
    AppendF32(&body, move.rotation.x);
    AppendF32(&body, move.rotation.y);
    AppendF32(&body, move.rotation.z);
    AppendF32(&body, move.rotation.w);
    AppendF32(&body, move.scale);
  }

  out->clear();
  out->insert(out->end(), std::begin(kMagic), std::end(kMagic));
  AppendU32(out, kVersion);
  AppendU32(out, 0);  // flags, reserved
  AppendU64(out, bake_id_);
  AppendU32(out, static_cast<u32>(destroyed_.size()));
  AppendU32(out, static_cast<u32>(moves_.size()));
  AppendU64(out, Checksum(std::span<const u8>(body.data(), body.size())));
  out->insert(out->end(), body.begin(), body.end());
  return true;
}

bool WorldOverlay::Decode(std::span<const u8> bytes, WorldOverlay* out, std::string* error) {
  if (!out) return false;
  Cursor header(bytes);
  const u8* magic = nullptr;
  if (!header.Take(sizeof(kMagic), &magic) || std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
    SetError(error, "world overlay: not an RXOVRLAY file");
    return false;
  }
  const u32 version = header.U32();
  if (version != kVersion) {
    SetError(error, "world overlay: version " + std::to_string(version) + ", expected " +
                        std::to_string(kVersion));
    return false;
  }
  header.U32();  // flags, reserved
  const u64 bake_id = header.U64();
  const u32 destroyed_count = header.U32();
  const u32 move_count = header.U32();
  const u64 checksum = header.U64();
  if (!header.ok()) {
    SetError(error, "world overlay: truncated header");
    return false;
  }
  if (destroyed_count > kMaximumDestroyed || move_count > kMaximumMoves) {
    SetError(error, "world overlay: header counts out of range");
    return false;
  }

  const size_t body_offset = header.offset();
  const u64 expected = static_cast<u64>(destroyed_count) * kDestroyedBytes +
                       static_cast<u64>(move_count) * kMoveBytes;
  if (bytes.size() - body_offset != expected) {
    SetError(error, "world overlay: body is " + std::to_string(bytes.size() - body_offset) +
                        " bytes, header describes " + std::to_string(expected));
    return false;
  }
  const std::span<const u8> body = bytes.subspan(body_offset);
  if (Checksum(body) != checksum) {
    SetError(error, "world overlay: checksum mismatch");
    return false;
  }

  out->Clear();
  out->set_bake_id(bake_id);
  Cursor cursor(body);
  out->destroyed_.reserve(destroyed_count);
  for (u32 i = 0; i < destroyed_count; ++i) {
    const u64 id = cursor.U64();
    // Sortedness is what makes IsDestroyed a binary search; a file that lost it
    // would answer wrongly rather than slowly.
    if (i > 0 && id <= out->destroyed_[i - 1]) {
      SetError(error, "world overlay: destroyed ids are not sorted at " + std::to_string(i));
      return false;
    }
    out->destroyed_.push_back(id);
  }
  out->moves_.reserve(move_count);
  for (u32 i = 0; i < move_count; ++i) {
    OverlayMove move;
    move.stable_id = cursor.U64();
    move.position.x = cursor.F32();
    move.position.y = cursor.F32();
    move.position.z = cursor.F32();
    move.rotation.x = cursor.F32();
    move.rotation.y = cursor.F32();
    move.rotation.z = cursor.F32();
    move.rotation.w = cursor.F32();
    move.scale = cursor.F32();
    if (i > 0 && move.stable_id <= out->moves_[i - 1].stable_id) {
      SetError(error, "world overlay: moves are not sorted at " + std::to_string(i));
      return false;
    }
    if (!Finite(move)) {
      SetError(error, "world overlay: move " + std::to_string(move.stable_id) +
                          " has a non-finite transform");
      return false;
    }
    if (out->IsDestroyed(move.stable_id)) {
      SetError(error, "world overlay: " + std::to_string(move.stable_id) +
                          " is both destroyed and moved");
      return false;
    }
    out->moves_.push_back(move);
  }
  if (!cursor.ok()) {
    SetError(error, "world overlay: truncated body");
    return false;
  }
  return true;
}

}  // namespace rx::world
