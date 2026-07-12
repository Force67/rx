#include "net/session.h"

#include <cmath>

#include "asset/asset_id.h"
#include "core/log.h"
#include "net/rpc_channel.h"
#include "net/znet_util.h"
#include "scene/components.h"

namespace rx::net {
namespace {

constexpr f32 kDefaultPlayerSpeed = 4.0f;  // units per second

f32 ClampAxis(f32 v) { return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v); }

// The stand-in player integration used when the game installs no simulator:
// axis-clamped fly movement plus yaw. Enough for demos and tests.
void DefaultSimulatePlayer(ecs::World& world, ecs::Entity player,
                           const PlayerInput& input, f32 dt) {
  scene::Transform* t = world.Get<scene::Transform>(player);
  if (!t) return;
  t->position[0] += ClampAxis(input.move_x) * kDefaultPlayerSpeed * dt;
  t->position[1] += ClampAxis(input.move_y) * kDefaultPlayerSpeed * dt;
  t->position[2] += ClampAxis(input.move_z) * kDefaultPlayerSpeed * dt;
  const f32 half_yaw = input.yaw * 0.5f;
  t->rotation[0] = 0;
  t->rotation[1] = std::sin(half_yaw);
  t->rotation[2] = 0;
  t->rotation[3] = std::cos(half_yaw);
}

}  // namespace

// --- server ---

ServerSession::ServerSession(SessionConfig config) : config_(std::move(config)) {
  interest_.Configure(config_.interest);
}

ServerSession::~ServerSession() {
  if (started_) server_.Deinit();
}

bool ServerSession::Start() {
  const tx::network::ZServer::StartOptions options{
      .use_encryption = false,
      .pre_shared_key = {},
      .use_compression = false,
      .allow_ipv6 = false,
      // Threaded mode: reliable retransmits and socket receive run in
      // zetanet's workers, the sim thread only touches lock-free queues.
      .start_threads = true,
      .chaos = {}};
  if (!server_.Begin(config_.port, options)) {
    RX_ERROR("net: failed to open server on port {}", config_.port);
    return false;
  }
  started_ = true;
  rpc_ = std::make_unique<RpcServerChannel>(server_);
  RX_INFO("net: server listening on {} (bubbles {})", config_.port,
          config_.bubble_radius > 0 ? "on" : "off");
  return true;
}

void ServerSession::Tick(ecs::World& world, f32 dt) {
  if (!started_) return;
  server_.Update();
  PollMessages(world);
  SimulatePlayers(world, dt);
  TimeoutClients(world, dt);
  ++tick_;
  if (tick_ % config_.snapshot_interval_ticks == 0) {
    SendSnapshots(world);
  }
  // Heartbeat for dedicated server logs, with the interest math that shows
  // what the bubbles saved.
  if (tick_ % (static_cast<u64>(config_.tick_rate) * 30) == 0) {
    if (config_.bubble_radius > 0 && stats_.broadcast_equiv_records > 0) {
      const f64 kept = static_cast<f64>(stats_.snapshot_records_sent) /
                       static_cast<f64>(stats_.broadcast_equiv_records);
      RX_INFO(
          "net: tick {}, {} clients, {} entities, sent {} records "
          "({:.0f}% of full visibility)",
          tick_, clients_.size(), world.entity_count(), stats_.snapshot_records_sent,
          kept * 100.0);
    } else {
      RX_INFO("net: tick {}, {} clients, {} entities", tick_, clients_.size(),
              world.entity_count());
    }
  }
}

void ServerSession::PollMessages(ecs::World& world) {
  tx::network::IncomingPacket packet;
  while (server_.Poll(tx::network::PacketChannelType::Data, packet)) {
    if (tx::network::IsSystemMessage(packet.type)) continue;
    const u32 peer = packet.source_peer_id;
    if (RemoteClient* client = clients_.find(peer)) {
      client->since_last_packet = 0;
    }
    const u16 type = static_cast<u16>(packet.type);
    if (type >= kFirstGameMessage) {
      if (game_message_sink_) {
        game_message_sink_(peer, type, PacketData(packet), packet.data.size());
      }
      continue;
    }
    switch (static_cast<MessageType>(type)) {
      case MessageType::kClientJoin: {
        if (auto join = ClientJoin::Decode(PacketData(packet), packet.data.size())) {
          HandleJoin(world, peer, *join);
        }
        break;
      }
      case MessageType::kPlayerInput: {
        RemoteClient* client = clients_.find(peer);
        if (!client) break;
        if (auto input = PlayerInput::Decode(PacketData(packet), packet.data.size())) {
          // Inputs are unreliable and unordered, keep only the newest.
          if (input->client_tick >= client->input.client_tick) {
            client->input = *input;
          }
        }
        break;
      }
      case MessageType::kRpcCall: {
        rpc_->OnPacket(peer, PacketData(packet), packet.data.size());
        break;
      }
      default:
        RX_WARN("net: unhandled engine message type {} from peer {}", type, peer);
        break;
    }
  }
}

void ServerSession::HandleJoin(ecs::World& world, u32 peer, const ClientJoin& join) {
  if (join.protocol != config_.protocol) {
    JoinRefuse refuse;
    refuse.reason = DisconnectReason::kProtocolMismatch;
    refuse.detail = "protocol version mismatch";
    server_.Push(MakePacket(peer, MessageType::kJoinRefuse, refuse.Encode(),
                            /*reliable=*/true, tx::network::PacketPriority::High));
    return;
  }

  RemoteClient* client = clients_.find(peer);
  const bool is_new_client = client == nullptr;
  if (!client) {
    if (clients_.size() >= config_.max_clients) {
      JoinRefuse refuse;
      refuse.reason = DisconnectReason::kServerFull;
      refuse.detail = "server full";
      server_.Push(MakePacket(peer, MessageType::kJoinRefuse, refuse.Encode(),
                              /*reliable=*/true, tx::network::PacketPriority::High));
      return;
    }

    RemoteClient fresh;
    fresh.name = base::NameString(join.player_name.data(), join.player_name.size());
    // The player entity is server-owned and replicates like anything else.
    const NetworkId net_id = AllocateNetworkId();
    scene::Transform spawn;
    spawn.position[0] = 1.5f * static_cast<f32>(clients_.size() + 1);
    spawn.position[2] = 1.5f;
    fresh.player = world.Create();
    world.Add(fresh.player, spawn);
    world.Add(fresh.player, net_id);
    if (config_.player_mesh != 0) {
      world.Add(fresh.player, scene::Renderable{asset::AssetId{config_.player_mesh}});
    }
    if (config_.bubble_radius > 0) {
      world.Add(fresh.player, InterestBubble{peer, config_.bubble_radius});
    }
    fresh.player_net_id = net_id.value;
    clients_.insert(peer, std::move(fresh));
    client = clients_.find(peer);
    if (player_spawn_sink_) player_spawn_sink_(world, client->player, peer);
    RX_INFO("net: peer {} joined as '{}' ({} clients)", peer, client->name.c_str(),
            clients_.size());
    // Make sure the newcomer gets the whole (relevant) world on the next
    // broadcast.
    force_keyframe_ = true;
  }

  // Reply (again, on duplicate joins from retransmits): the accept itself
  // can get lost even on the reliable path if the ack raced a timeout.
  JoinAccept accept;
  accept.player_entity = client->player_net_id;
  accept.server_tick = tick_;
  accept.protocol = config_.protocol;
  accept.client_id = peer;
  accept.tick_rate = static_cast<u16>(config_.tick_rate);
  accept.snapshot_rate =
      static_cast<u16>(config_.tick_rate / config_.snapshot_interval_ticks);
  server_.Push(MakePacket(peer, MessageType::kJoinAccept, accept.Encode(),
                          /*reliable=*/true, tx::network::PacketPriority::High));

  // Fire the join hook once, for every new peer.
  if (is_new_client && client_joined_sink_) client_joined_sink_(peer);
}

void ServerSession::DropClient(ecs::World& world, u32 peer) {
  RemoteClient* client = clients_.find(peer);
  if (!client) return;
  RX_INFO("net: dropping peer {} ('{}')", peer, client->name.c_str());
  if (world.IsAlive(client->player)) world.Destroy(client->player);
  interest_.RemovePeer(peer);
  clients_.erase(peer);
  if (client_left_sink_) client_left_sink_(peer);
}

void ServerSession::SimulatePlayers(ecs::World& world, f32 dt) {
  for (auto entry : clients_) {
    RemoteClient& client = entry.value;
    if (!world.IsAlive(client.player)) continue;
    if (player_simulator_) {
      player_simulator_(world, client.player, client.input, dt);
    } else {
      DefaultSimulatePlayer(world, client.player, client.input, dt);
    }
  }
}

void ServerSession::TimeoutClients(ecs::World& world, f32 dt) {
  scratch_dropped_.clear();
  for (auto entry : clients_) {
    entry.value.since_last_packet += dt;
    if (entry.value.since_last_packet > config_.client_timeout_seconds) {
      scratch_dropped_.push_back(entry.key);
    }
  }
  for (u32 peer : scratch_dropped_) DropClient(world, peer);
}

void ServerSession::SendSnapshots(ecs::World& world) {
  const bool bubbles_on = config_.bubble_radius > 0;
  capture_.Capture(world, tick_, hooks_);
  if (bubbles_on) interest_.Update(world, tick_);

  if (clients_.size() == 0) return;

  const bool full = force_keyframe_ || (tick_ % config_.keyframe_interval_ticks == 0);
  force_keyframe_ = false;

  // One delta stream per client over the shared capture: each peer gets only
  // its bubble's slice of the world, with despawns covering whatever slid out.
  for (auto entry : clients_) {
    RemoteClient& client = entry.value;
    const InterestSet* interest =
        bubbles_on ? interest_.InterestOf(entry.key) : nullptr;
    const u32 written = client.stream.Build(capture_, interest, full, &snapshot_);
    stats_.broadcast_equiv_records += capture_.entities().size();
    if (written == 0 && snapshot_.despawned.empty() && !full) continue;

    // Snapshots stay unreliable by design: the next one supersedes a lost one
    // and keyframes repair anything structural.
    std::vector<u8> blob = snapshot_.Encode();
    stats_.snapshots_sent += 1;
    stats_.snapshot_records_sent += written;
    stats_.snapshot_bytes_sent += blob.size();
    server_.Push(MakePacket(entry.key, MessageType::kSnapshot, blob,
                            /*reliable=*/false,
                            full ? tx::network::PacketPriority::High
                                 : tx::network::PacketPriority::Medium));
  }

  // Everyone learns where every bubble is (tiny, and it drives remote
  // visualizers/HUDs). Unreliable: the next sync supersedes a lost one.
  if (bubbles_on && interest_.bubbles().size() > 0) {
    server_.Push(MakePacket(tx::network::ZPeerId::to_all, MessageType::kBubbleSync,
                            EncodeBubbleSync(interest_.bubbles()),
                            /*reliable=*/false, tx::network::PacketPriority::Low));
  }
}

void ServerSession::SendTo(u32 peer, u16 type, const std::vector<u8>& payload,
                           bool reliable, tx::network::PacketPriority priority) {
  server_.Push(MakePacket(peer, type, payload, reliable, priority));
}

void ServerSession::Broadcast(u16 type, const std::vector<u8>& payload, bool reliable,
                              tx::network::PacketPriority priority) {
  if (clients_.size() == 0) return;
  server_.Push(MakePacket(tx::network::ZPeerId::to_all, type, payload, reliable, priority));
}

ecs::Entity ServerSession::PlayerOf(u32 peer) const {
  const RemoteClient* client = clients_.find(peer);
  return client ? client->player : ecs::kInvalidEntity;
}

// --- client ---

ClientSession::ClientSession(SessionConfig config) : config_(std::move(config)) {
  if (config_.snapshot_interval_ticks > 0 && config_.tick_rate > 0) {
    snapshot_dt_ = static_cast<f32>(config_.snapshot_interval_ticks) /
                   static_cast<f32>(config_.tick_rate);
  }
}

ClientSession::~ClientSession() {
  client_.Disconnect();
}

bool ClientSession::Start() {
  const tx::network::ZClient::ConnectionOptions options{
      .use_encryption = false,
      .pre_shared_key = {},
      .use_compression = false,
      .allow_ipv6 = false,
      .start_threads = true,
      .chaos = {}};
  if (!client_.Connect(base::StringRef(config_.address.c_str()), config_.port,
                       options)) {
    RX_ERROR("net: failed to start connecting to {}:{}", config_.address.c_str(),
             config_.port);
    return false;
  }
  rpc_ = std::make_unique<RpcClientChannel>(client_);
  RX_INFO("net: connecting to {}:{}", config_.address.c_str(), config_.port);
  return true;
}

void ClientSession::Tick(ecs::World& world, f32 dt) {
  // Drain the control channel first when a file sink is installed: zetanet
  // delivers file-transfer chunks as system packets there, and
  // ZClient::Update() would otherwise drain and discard them. Poll() still
  // runs ProcessSystemMessage for handshake/clock packets, so the connection
  // logic below is unaffected.
  if (file_packet_sink_) {
    tx::network::IncomingPacket fpacket;
    while (client_.Poll(tx::network::PacketChannelType::Control, fpacket)) {
      if (fpacket.type == tx::network::PacketType::FileTransfer) {
        file_packet_sink_(fpacket);
      }
    }
  }
  client_.Update();

  using Phase = tx::network::ZClient::HandshakePhase;
  const Phase phase = client_.handshake_phase();
  if (phase == Phase::kFailed) {
    if (!failure_logged_) {
      RX_ERROR("net: handshake failed (reason {})",
               static_cast<u32>(client_.handshake_failure_reason()));
      failure_logged_ = true;
      applier_.Reset(world);
      joined_ = false;
    }
    return;
  }
  if (phase == Phase::kConnected && !join_sent_) {
    ClientJoin join;
    join.protocol = config_.protocol;
    join.player_name.assign(config_.player_name.data(), config_.player_name.size());
    client_.Push(MakePacket(tx::network::ZPeerId::to_server, MessageType::kClientJoin,
                            join.Encode(), /*reliable=*/true,
                            tx::network::PacketPriority::High));
    join_sent_ = true;
  }

  PollMessages(world);

  if (joined_) {
    input_.client_tick = tick_;
    client_.Push(MakePacket(tx::network::ZPeerId::to_server, MessageType::kPlayerInput,
                            input_.Encode(), /*reliable=*/false));

    // Heartbeat for headless clients.
    if (tick_ % (static_cast<u64>(config_.tick_rate) * 5) == 0) {
      const ecs::Entity player = player_entity();
      if (const auto* t = player ? world.Get<scene::Transform>(player) : nullptr) {
        RX_INFO("net: {} replicated entities, player at ({:.2f}, {:.2f}, {:.2f})",
                applier_.entity_count(), t->position[0], t->position[1],
                t->position[2]);
      } else {
        RX_INFO("net: {} replicated entities, player not spawned yet",
                applier_.entity_count());
      }
    }
  }
  ++tick_;
}

void ClientSession::SendToServer(u16 type, const std::vector<u8>& payload, bool reliable,
                                 tx::network::PacketPriority priority) {
  client_.Push(MakePacket(tx::network::ZPeerId::to_server, type, payload, reliable, priority));
}

void ClientSession::PollMessages(ecs::World& world) {
  tx::network::IncomingPacket packet;
  while (client_.Poll(tx::network::PacketChannelType::Data, packet)) {
    if (tx::network::IsSystemMessage(packet.type)) continue;
    const u16 type = static_cast<u16>(packet.type);
    if (type >= kFirstGameMessage) {
      if (game_message_sink_) {
        game_message_sink_(type, PacketData(packet), packet.data.size());
      }
      continue;
    }
    switch (static_cast<MessageType>(type)) {
      case MessageType::kJoinAccept: {
        auto accept = JoinAccept::Decode(PacketData(packet), packet.data.size());
        if (!accept) break;
        joined_ = true;
        player_net_id_ = accept->player_entity;
        if (accept->snapshot_rate > 0) {
          snapshot_dt_ = 1.0f / static_cast<f32>(accept->snapshot_rate);
        }
        RX_INFO("net: joined as client {} (player entity {})", accept->client_id,
                player_net_id_);
        if (joined_sink_) joined_sink_(*accept);
        break;
      }
      case MessageType::kJoinRefuse: {
        if (auto refuse = JoinRefuse::Decode(PacketData(packet), packet.data.size())) {
          RX_ERROR("net: server refused join: {}", refuse->detail.c_str());
        }
        joined_ = false;
        failure_logged_ = true;
        client_.Disconnect();
        return;
      }
      case MessageType::kSnapshot: {
        if (auto snapshot = Snapshot::Decode(PacketData(packet), packet.data.size())) {
          applier_.Apply(world, *snapshot, snapshot_dt_, hooks_);
        }
        break;
      }
      case MessageType::kBubbleSync: {
        if (auto bubbles = DecodeBubbleSync(PacketData(packet), packet.data.size())) {
          bubbles_ = std::move(*bubbles);
        }
        break;
      }
      case MessageType::kRpcCall: {
        rpc_->OnPacket(PacketData(packet), packet.data.size());
        break;
      }
      default:
        RX_WARN("net: unhandled engine message type {}", type);
        break;
    }
  }
}

}  // namespace rx::net
