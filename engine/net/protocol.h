#ifndef RX_NET_PROTOCOL_H_
#define RX_NET_PROTOCOL_H_

#include <optional>
#include <string>
#include <vector>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"

namespace rx::net {

// Application packet ids. The transport (zetanet) owns everything below 100;
// the engine session protocol owns [101, kFirstGameMessage); a game's own
// messages start at kFirstGameMessage and reach the session's game-message
// sink undecoded.
enum class MessageType : u16 {
  kClientJoin = 101,   // client -> server: join once the transport handshake is up
  kJoinAccept = 102,   // server -> client: admitted, session parameters
  kJoinRefuse = 103,   // server -> client: turned away
  kSnapshot = 104,     // server -> client: that client's entity stream
  kPlayerInput = 105,  // client -> server: input for its player entity
  kRpcCall = 106,      // either direction: an encoded scripting RPC (rpc::EncodeCall)
  kBubbleSync = 107,   // server -> clients: every player's interest bubble (debug/HUD)
};

// First message id available to the embedding game. Ids at or above this are
// never interpreted by the engine session; they surface through the
// game-message sinks.
inline constexpr u16 kFirstGameMessage = 128;

// Why the server refused a join or dropped a client.
enum class DisconnectReason : u8 {
  kUnknown = 0,
  kShutdown = 1,
  kKicked = 2,
  kProtocolMismatch = 3,
  kServerFull = 4,
};

// First data-channel message a client sends once the transport handshake is
// connected. `protocol` is the game's wire version (SessionConfig::protocol);
// both sides must agree on it, engine and game payloads alike.
struct RX_NET_EXPORT ClientJoin {
  u32 protocol = 0;
  std::string player_name;

  std::vector<u8> Encode() const;
  static std::optional<ClientJoin> Decode(const u8* data, size_t size);
};

// Server reply admitting a client to the session.
struct RX_NET_EXPORT JoinAccept {
  u64 player_entity = 0;  // the joiner's player NetworkId
  u64 server_tick = 0;
  u32 protocol = 0;
  u32 client_id = 0;  // the transport peer id, the client's identity in RPCs
  u16 tick_rate = 60;
  u16 snapshot_rate = 20;

  std::vector<u8> Encode() const;
  static std::optional<JoinAccept> Decode(const u8* data, size_t size);
};

// Server reply turning a client away.
struct RX_NET_EXPORT JoinRefuse {
  DisconnectReason reason = DisconnectReason::kUnknown;
  std::string detail;

  std::vector<u8> Encode() const;
  static std::optional<JoinRefuse> Decode(const u8* data, size_t size);
};

// State of one replicated entity: the transform plus two opaque payload slots.
// `mesh` is the AssetId hash the replica renders with (0 = none); `user_tag`
// is the game's per-entity payload (0 = none) captured and applied through
// ReplicationHooks -- recreation packs its Bethesda form id there.
struct RX_NET_EXPORT EntityState {
  u64 net_id = 0;
  u64 mesh = 0;
  u64 user_tag = 0;
  f32 position[3] = {0, 0, 0};
  f32 rotation[4] = {0, 0, 0, 1};
  f32 scale = 1.0f;

  bool operator==(const EntityState&) const = default;
};

// Server-to-client entity stream. A full snapshot carries every entity in the
// client's interest and the client despawns anything absent; a delta carries
// only entities that changed since the previous build for that client.
struct RX_NET_EXPORT Snapshot {
  u64 server_tick = 0;
  bool full = false;
  base::Vector<EntityState> entities;
  base::Vector<u64> despawned;

  std::vector<u8> Encode() const;
  static std::optional<Snapshot> Decode(const u8* data, size_t size);
};

// Client-to-server input for its player entity.
struct RX_NET_EXPORT PlayerInput {
  u64 client_tick = 0;
  f32 move_x = 0;
  f32 move_y = 0;
  f32 move_z = 0;
  f32 yaw = 0;
  f32 pitch = 0;
  u32 buttons = 0;

  std::vector<u8> Encode() const;
  static std::optional<PlayerInput> Decode(const u8* data, size_t size);
};

// One player's interest bubble as the server sees it, replicated to every
// client (kBubbleSync) so HUDs and the 3D visualizer can draw the session's
// bubbles anywhere, not just on the host.
struct RX_NET_EXPORT BubbleState {
  u32 peer = 0;
  f32 center[3] = {0, 0, 0};
  f32 radius = 0;
  u32 entity_count = 0;  // replicated entities inside this bubble
  u32 owned_count = 0;   // of those, entities this peer owns

  bool operator==(const BubbleState&) const = default;
};

RX_NET_EXPORT std::vector<u8> EncodeBubbleSync(const base::Vector<BubbleState>& bubbles);
RX_NET_EXPORT std::optional<base::Vector<BubbleState>> DecodeBubbleSync(const u8* data,
                                                                        size_t size);

}  // namespace rx::net

#endif  // RX_NET_PROTOCOL_H_
