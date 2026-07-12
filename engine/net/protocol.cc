#include "net/protocol.h"

#include "net/wire.h"

namespace rx::net {
namespace {

// Snapshot entity records and bubble states are fixed-size, so the list
// counts up front let the decoder reject a lying header before reading.
constexpr size_t kEntityRecordBytes = 8 + 8 + 8 + 8 * 4;  // ids/tags + 8 f32
constexpr size_t kBubbleRecordBytes = 4 + 4 * 4 + 4 + 4;

void PutEntityState(ByteWriter& w, const EntityState& e) {
  w.U64(e.net_id);
  w.U64(e.mesh);
  w.U64(e.user_tag);
  for (f32 v : e.position) w.F32(v);
  for (f32 v : e.rotation) w.F32(v);
  w.F32(e.scale);
}

EntityState TakeEntityState(ByteReader& r) {
  EntityState e;
  e.net_id = r.U64();
  e.mesh = r.U64();
  e.user_tag = r.U64();
  for (f32& v : e.position) v = r.F32();
  for (f32& v : e.rotation) v = r.F32();
  e.scale = r.F32();
  return e;
}

}  // namespace

std::vector<u8> ClientJoin::Encode() const {
  ByteWriter w;
  w.U32(protocol);
  w.Str(player_name);
  return w.Take();
}

std::optional<ClientJoin> ClientJoin::Decode(const u8* data, size_t size) {
  ByteReader r(data, size);
  ClientJoin m;
  m.protocol = r.U32();
  m.player_name = r.Str();
  if (!r.ok()) return std::nullopt;
  return m;
}

std::vector<u8> JoinAccept::Encode() const {
  ByteWriter w;
  w.U64(player_entity);
  w.U64(server_tick);
  w.U32(protocol);
  w.U32(client_id);
  w.U16(tick_rate);
  w.U16(snapshot_rate);
  return w.Take();
}

std::optional<JoinAccept> JoinAccept::Decode(const u8* data, size_t size) {
  ByteReader r(data, size);
  JoinAccept m;
  m.player_entity = r.U64();
  m.server_tick = r.U64();
  m.protocol = r.U32();
  m.client_id = r.U32();
  m.tick_rate = r.U16();
  m.snapshot_rate = r.U16();
  if (!r.ok()) return std::nullopt;
  return m;
}

std::vector<u8> JoinRefuse::Encode() const {
  ByteWriter w;
  w.U8(static_cast<u8>(reason));
  w.Str(detail);
  return w.Take();
}

std::optional<JoinRefuse> JoinRefuse::Decode(const u8* data, size_t size) {
  ByteReader r(data, size);
  JoinRefuse m;
  m.reason = static_cast<DisconnectReason>(r.U8());
  m.detail = r.Str();
  if (!r.ok()) return std::nullopt;
  return m;
}

std::vector<u8> Snapshot::Encode() const {
  ByteWriter w;
  w.U64(server_tick);
  w.Bool(full);
  w.U32(static_cast<u32>(entities.size()));
  for (const EntityState& e : entities) PutEntityState(w, e);
  w.U32(static_cast<u32>(despawned.size()));
  for (u64 id : despawned) w.U64(id);
  return w.Take();
}

std::optional<Snapshot> Snapshot::Decode(const u8* data, size_t size) {
  ByteReader r(data, size);
  Snapshot m;
  m.server_tick = r.U64();
  m.full = r.Bool();
  const u32 entity_count = r.U32();
  if (static_cast<size_t>(entity_count) * kEntityRecordBytes > r.remaining()) {
    return std::nullopt;
  }
  m.entities.reserve(entity_count);
  for (u32 i = 0; i < entity_count; ++i) m.entities.push_back(TakeEntityState(r));
  const u32 despawn_count = r.U32();
  if (static_cast<size_t>(despawn_count) * 8 > r.remaining()) return std::nullopt;
  m.despawned.reserve(despawn_count);
  for (u32 i = 0; i < despawn_count; ++i) m.despawned.push_back(r.U64());
  if (!r.ok()) return std::nullopt;
  return m;
}

std::vector<u8> PlayerInput::Encode() const {
  ByteWriter w;
  w.U64(client_tick);
  w.F32(move_x);
  w.F32(move_y);
  w.F32(move_z);
  w.F32(yaw);
  w.F32(pitch);
  w.U32(buttons);
  return w.Take();
}

std::optional<PlayerInput> PlayerInput::Decode(const u8* data, size_t size) {
  ByteReader r(data, size);
  PlayerInput m;
  m.client_tick = r.U64();
  m.move_x = r.F32();
  m.move_y = r.F32();
  m.move_z = r.F32();
  m.yaw = r.F32();
  m.pitch = r.F32();
  m.buttons = r.U32();
  if (!r.ok()) return std::nullopt;
  return m;
}

std::vector<u8> EncodeBubbleSync(const base::Vector<BubbleState>& bubbles) {
  ByteWriter w;
  w.U32(static_cast<u32>(bubbles.size()));
  for (const BubbleState& b : bubbles) {
    w.U32(b.peer);
    for (f32 v : b.center) w.F32(v);
    w.F32(b.radius);
    w.U32(b.entity_count);
    w.U32(b.owned_count);
  }
  return w.Take();
}

std::optional<base::Vector<BubbleState>> DecodeBubbleSync(const u8* data, size_t size) {
  ByteReader r(data, size);
  const u32 count = r.U32();
  if (static_cast<size_t>(count) * kBubbleRecordBytes > r.remaining()) {
    return std::nullopt;
  }
  base::Vector<BubbleState> out;
  out.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    BubbleState b;
    b.peer = r.U32();
    for (f32& v : b.center) v = r.F32();
    b.radius = r.F32();
    b.entity_count = r.U32();
    b.owned_count = r.U32();
    out.push_back(b);
  }
  if (!r.ok()) return std::nullopt;
  return out;
}

}  // namespace rx::net
