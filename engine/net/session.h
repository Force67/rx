#ifndef RX_NET_SESSION_H_
#define RX_NET_SESSION_H_

// Client/server sessions over zetanet. Compiled only when the transport
// target is present (RX_NET_HAS_ZETANET); the codec, replication and bubble
// halves of rx::net have no transport dependency.

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/strings/xstring.h>

#include <znet/z_client.h>
#include <znet/z_server.h>


#include "base/containers/vector.h"
#include "base/functional/function.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "core/export.h"
#include "ecs/world.h"
#include "net/bubble.h"
#include "net/protocol.h"
#include "net/replication.h"

namespace rx::net {

class RpcServerChannel;
class RpcClientChannel;

struct SessionConfig {
  u16 port = 29700;
  base::String address;  // client: server to join
  base::NameString player_name{"player"};
  // The game's wire version, checked at join. Bump it whenever any payload,
  // engine or game, changes shape.
  u32 protocol = 1;
  u32 max_clients = 64;
  u32 tick_rate = 60;
  u32 snapshot_interval_ticks = 3;   // 20 Hz at the 60 Hz fixed step
  u32 keyframe_interval_ticks = 60;  // full snapshot every second
  f32 client_timeout_seconds = 10.0f;
  u64 player_mesh = 0;  // AssetId hash spawned for joining players

  // Streaming bubbles: when the radius is positive every joining player gets
  // an InterestBubble and the server streams each client only the entities
  // inside its bubble (per-peer delta streams instead of a broadcast), the
  // bandwidth lever that scales the player count. 0 replicates everything to
  // everyone, exactly the pre-bubble behavior.
  f32 bubble_radius = 0;
  InterestConfig interest;
};

// Aggregate send-side counters for one session, the numbers behind the
// bubbles' bandwidth win. broadcast-equivalent = what full visibility would
// have cost this session (every capture record to every client).
struct NetStats {
  u64 snapshots_sent = 0;
  u64 snapshot_records_sent = 0;
  u64 snapshot_bytes_sent = 0;
  u64 broadcast_equiv_records = 0;
};

// The server simulates, clients render what snapshots tell them. One process
// can run both (listen server) since sessions only touch the world through
// the ECS.
class RX_REPLICATION_EXPORT Session {
 public:
  virtual ~Session() = default;

  // Called from the fixed-step sim stage.
  virtual void Tick(ecs::World& world, f32 dt) = 0;
};

class RX_REPLICATION_EXPORT ServerSession : public Session {
 public:
  explicit ServerSession(SessionConfig config);
  ~ServerSession() override;

  bool Start();
  void Tick(ecs::World& world, f32 dt) override;

  // game seams

  // Every data-channel message with id >= kFirstGameMessage lands here,
  // undecoded, attributed to its peer. Unset drops them.
  void SetGameMessageSink(
      base::Function<void(u32 peer, u16 type, const u8* data, size_t size)> sink) {
    game_message_sink_ = base::move(sink);
  }

  // Called after the server spawns a joiner's player entity (Transform +
  // NetworkId + optional Renderable + InterestBubble already attached), so
  // the game adds its own components or moves the spawn.
  void SetPlayerSpawnSink(base::Function<void(ecs::World&, ecs::Entity, u32 peer)> sink) {
    player_spawn_sink_ = base::move(sink);
  }

  // Integrates one player's newest input each fixed step. Unset runs a plain
  // fly-move (demo default); a game replaces it with its actual locomotion.
  void SetPlayerSimulator(
      base::Function<void(ecs::World&, ecs::Entity, const PlayerInput&, f32 dt)> sim) {
    player_simulator_ = base::move(sim);
  }

  // Per-entity replication payload (user tags) capture, and (through the
  // same hooks type) what a client does when a tagged replica spawns.
  void SetReplicationHooks(ReplicationHooks hooks) { hooks_ = base::move(hooks); }

  // Join/leave notifications, the fundamental multiplayer hooks for
  // server-side game logic. Fired for every peer; unset sinks drop the notice.
  void SetClientJoinedSink(base::Function<void(u32)> sink) {
    client_joined_sink_ = base::move(sink);
  }
  void SetClientLeftSink(base::Function<void(u32)> sink) {
    client_left_sink_ = base::move(sink);
  }

  // sending

  // Ships a game payload to one client / every client on the data channel.
  void SendTo(u32 peer, u16 type, const base::Vector<u8>& payload, bool reliable,
              tx::network::PacketPriority priority = tx::network::PacketPriority::Medium);
  void Broadcast(u16 type, const base::Vector<u8>& payload, bool reliable,
                 tx::network::PacketPriority priority = tx::network::PacketPriority::Medium);

  // The server's scripting RPC channel. Always present once Start succeeds.
  RpcServerChannel* rpc() { return rpc_.Get_UseOnlyIfYouKnowWhatYouareDoing(); }

  // Escape hatch for game layers that ride zetanet features directly (file
  // transfer, manifests): the raw transport. Everything sent through it must
  // use ids >= kFirstGameMessage.
  tx::network::ZServer& raw() { return server_; }

  // The interest map (bubbles, membership, ownership) as of the last
  // snapshot tick. Empty when bubbles are off.
  const InterestMap& interest() const { return interest_; }
  InterestMap& interest() { return interest_; }

  const NetStats& stats() const { return stats_; }
  u32 client_count() const { return static_cast<u32>(clients_.size()); }
  u64 tick() const { return tick_; }
  ecs::Entity PlayerOf(u32 peer) const;
  // The player entity's network id, or 0 when the peer is unknown.
  u64 PlayerNetId(u32 peer) const;

  // Visits every admitted peer id (game layers pushing per-peer payloads,
  // e.g. re-offering an asset manifest after a live reload).
  void ForEachPeer(const base::Function<void(u32 peer)>& fn) const {
    for (auto entry : clients_) fn(entry.key);
  }

 private:
  struct RemoteClient {
    base::NameString name;
    ecs::Entity player = ecs::kInvalidEntity;
    u64 player_net_id = 0;
    PlayerInput input;
    f32 since_last_packet = 0;
    PeerStream stream;
  };

  void PollMessages(ecs::World& world);
  void HandleJoin(ecs::World& world, u32 peer, const ClientJoin& join);
  void DropClient(ecs::World& world, u32 peer);
  void SimulatePlayers(ecs::World& world, f32 dt);
  void TimeoutClients(ecs::World& world, f32 dt);
  void SendSnapshots(ecs::World& world);

  SessionConfig config_;
  tx::network::ZServer server_;
  WorldCapture capture_;
  InterestMap interest_;
  Snapshot snapshot_;  // reused so the vectors keep their capacity
  base::UnorderedMap<u32, RemoteClient> clients_;
  base::Vector<u32> scratch_dropped_;
  ReplicationHooks hooks_;
  base::Function<void(u32, u16, const u8*, size_t)> game_message_sink_;
  base::Function<void(ecs::World&, ecs::Entity, u32)> player_spawn_sink_;
  base::Function<void(ecs::World&, ecs::Entity, const PlayerInput&, f32)> player_simulator_;
  base::Function<void(u32)> client_joined_sink_;
  base::Function<void(u32)> client_left_sink_;
  base::UniquePointer<RpcServerChannel> rpc_;
  NetStats stats_;
  u64 tick_ = 0;
  bool force_keyframe_ = false;
  bool started_ = false;
};

class RX_REPLICATION_EXPORT ClientSession : public Session {
 public:
  explicit ClientSession(SessionConfig config);
  ~ClientSession() override;

  bool Start();
  void Tick(ecs::World& world, f32 dt) override;

  // Local input forwarded to the server every tick once joined.
  void SetInput(const PlayerInput& input) { input_ = input; }

  // Every data-channel message with id >= kFirstGameMessage lands here,
  // undecoded. Unset drops them.
  void SetGameMessageSink(base::Function<void(u16 type, const u8* data, size_t size)> sink) {
    game_message_sink_ = base::move(sink);
  }

  // What a replica spawn with a user tag means to this game (hooks are the
  // client half of ReplicationHooks; capture is unused here).
  void SetReplicationHooks(ReplicationHooks hooks) { hooks_ = base::move(hooks); }

  // Control-channel packets zetanet classifies as file transfers, polled
  // before Update() would discard them. A game's asset streaming taps here.
  void SetFilePacketSink(base::Function<void(const tx::network::IncomingPacket&)> sink) {
    file_packet_sink_ = base::move(sink);
  }

  // Fired once when the server accepts the join.
  void SetJoinedSink(base::Function<void(const JoinAccept&)> sink) {
    joined_sink_ = base::move(sink);
  }

  // Ships a game payload to the server on the data channel.
  void SendToServer(u16 type, const base::Vector<u8>& payload, bool reliable,
                    tx::network::PacketPriority priority = tx::network::PacketPriority::Medium);

  // The client's scripting RPC channel. Always present once Start succeeds.
  RpcClientChannel* rpc() { return rpc_.Get_UseOnlyIfYouKnowWhatYouareDoing(); }

  // Escape hatch: the raw transport (see ServerSession::raw).
  tx::network::ZClient& raw() { return client_; }

  // The whole session's bubbles as last replicated by the server
  // (kBubbleSync). Feed them to a BubbleVisualizer or a HUD. Empty when the
  // server runs without bubbles.
  const base::Vector<BubbleState>& bubbles() const { return bubbles_; }

  bool joined() const { return joined_; }
  u64 player_net_id() const { return player_net_id_; }
  ecs::Entity player_entity() const { return replicated_entity(player_net_id_); }
  // Any replicated entity by its network id, or kInvalidEntity when the
  // snapshot stream has not (yet) spawned it.
  ecs::Entity replicated_entity(u64 net_id) const { return applier_.Find(net_id); }
  u32 replicated_entity_count() const { return applier_.entity_count(); }

 private:
  void PollMessages(ecs::World& world);

  SessionConfig config_;
  tx::network::ZClient client_;
  SnapshotApplier applier_;
  ReplicationHooks hooks_;
  base::UniquePointer<RpcClientChannel> rpc_;
  base::Function<void(u16, const u8*, size_t)> game_message_sink_;
  base::Function<void(const tx::network::IncomingPacket&)> file_packet_sink_;
  base::Function<void(const JoinAccept&)> joined_sink_;
  base::Vector<BubbleState> bubbles_;
  PlayerInput input_;
  u64 player_net_id_ = 0;
  u64 tick_ = 0;
  f32 snapshot_dt_ = 1.0f / 20.0f;
  bool join_sent_ = false;
  bool joined_ = false;
  bool failure_logged_ = false;
};

}  // namespace rx::net

#endif  // RX_NET_SESSION_H_
