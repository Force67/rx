// rx::net acceptance: wire codec roundtrips, per-peer delta streams over one
// world capture, interest-set despawns, snapshot application, and the
// streaming bubbles (membership hysteresis + sticky deterministic ownership).
// Pure CPU logic, no GPU, driver or transport involved.

#include <cmath>
#include <cstdio>

#include "ecs/world.h"
#include "net/bubble.h"
#include "net/protocol.h"
#include "net/replication.h"
#include "net/wire.h"
#include "scene/components.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace net = rx::net;
namespace ecs = rx::ecs;
namespace scene = rx::scene;
using rx::f32;
using rx::u32;
using rx::u64;
using rx::u8;

net::EntityState MakeState(u64 net_id, f32 x, f32 y, f32 z) {
  net::EntityState s;
  s.net_id = net_id;
  s.position[0] = x;
  s.position[1] = y;
  s.position[2] = z;
  return s;
}

ecs::Entity Spawn(ecs::World& world, u64 net_id, f32 x, f32 y, f32 z) {
  ecs::Entity e = world.Create();
  scene::Transform t;
  t.position[0] = x;
  t.position[1] = y;
  t.position[2] = z;
  world.Add(e, t);
  world.Add(e, net::NetworkId{net_id});
  return e;
}

void MoveTo(ecs::World& world, ecs::Entity e, f32 x, f32 y, f32 z) {
  scene::Transform* t = world.Get<scene::Transform>(e);
  t->position[0] = x;
  t->position[1] = y;
  t->position[2] = z;
}

// --- wire + protocol ---

void TestWireRoundtrip() {
  net::ByteWriter w;
  w.U8(7);
  w.U16(65535);
  w.U32(0xdeadbeef);
  w.U64(0x0123456789abcdefull);
  w.F32(3.5f);
  w.Bool(true);
  w.Str("bubble");
  std::vector<u8> buf = w.Take();

  net::ByteReader r(buf.data(), buf.size());
  CHECK_EQ(r.U8(), 7);
  CHECK_EQ(r.U16(), 65535);
  CHECK_EQ(r.U32(), 0xdeadbeefu);
  CHECK_EQ(r.U64(), 0x0123456789abcdefull);
  CHECK_EQ(r.F32(), 3.5f);
  CHECK_EQ(r.Bool(), true);
  CHECK(r.Str() == "bubble");
  CHECK(r.ok());
  CHECK_EQ(r.remaining(), 0u);

  // Truncation turns into !ok, not UB.
  net::ByteReader short_read(buf.data(), 3);
  short_read.U8();
  short_read.U32();
  CHECK(!short_read.ok());
}

void TestProtocolRoundtrip() {
  {
    net::ClientJoin join;
    join.protocol = 42;
    join.player_name = "frank";
    auto decoded = net::ClientJoin::Decode(join.Encode().data(), join.Encode().size());
    CHECK(decoded && decoded->protocol == 42 && decoded->player_name == "frank");
  }
  {
    net::JoinAccept accept;
    accept.player_entity = 99;
    accept.server_tick = 1234;
    accept.client_id = 3;
    accept.snapshot_rate = 20;
    std::vector<u8> blob = accept.Encode();
    auto decoded = net::JoinAccept::Decode(blob.data(), blob.size());
    CHECK(decoded && decoded->player_entity == 99 && decoded->server_tick == 1234 &&
          decoded->client_id == 3 && decoded->snapshot_rate == 20);
    CHECK(!net::JoinAccept::Decode(blob.data(), blob.size() - 1));
  }
  {
    net::Snapshot snap;
    snap.server_tick = 77;
    snap.full = true;
    net::EntityState e = MakeState(5, 1, 2, 3);
    e.mesh = 0xabc;
    e.user_tag = 0xdef;
    snap.entities.push_back(e);
    snap.despawned.push_back(11);
    std::vector<u8> blob = snap.Encode();
    auto decoded = net::Snapshot::Decode(blob.data(), blob.size());
    CHECK(decoded && decoded->server_tick == 77 && decoded->full);
    CHECK(decoded->entities.size() == 1 && decoded->entities[0] == e);
    CHECK(decoded->despawned.size() == 1 && decoded->despawned[0] == 11);
    // A lying list count must not over-read.
    CHECK(!net::Snapshot::Decode(blob.data(), 16));
  }
  {
    base::Vector<net::BubbleState> bubbles;
    net::BubbleState b;
    b.peer = 2;
    b.center[0] = 10;
    b.radius = 32;
    b.entity_count = 7;
    b.owned_count = 4;
    bubbles.push_back(b);
    std::vector<u8> blob = net::EncodeBubbleSync(bubbles);
    auto decoded = net::DecodeBubbleSync(blob.data(), blob.size());
    CHECK(decoded && decoded->size() == 1 && (*decoded)[0] == b);
  }
  {
    net::PlayerInput input;
    input.client_tick = 9;
    input.move_x = -1;
    input.yaw = 0.5f;
    input.buttons = 3;
    std::vector<u8> blob = input.Encode();
    auto decoded = net::PlayerInput::Decode(blob.data(), blob.size());
    CHECK(decoded && decoded->client_tick == 9 && decoded->move_x == -1.0f &&
          decoded->yaw == 0.5f && decoded->buttons == 3);
  }
}

// --- per-peer delta streams ---

void TestPeerStreamDelta() {
  ecs::World world;
  ecs::Entity a = Spawn(world, 1, 0, 0, 0);
  Spawn(world, 2, 5, 0, 0);

  net::ReplicationHooks hooks;
  net::WorldCapture capture;
  net::PeerStream stream;
  net::Snapshot snap;

  capture.Capture(world, 10, hooks);
  CHECK_EQ(stream.Build(capture, nullptr, /*full=*/true, &snap), 2u);

  // Nothing moved: an empty delta.
  capture.Capture(world, 11, hooks);
  CHECK_EQ(stream.Build(capture, nullptr, /*full=*/false, &snap), 0u);
  CHECK(snap.despawned.empty());

  // One moved: a one-record delta.
  MoveTo(world, a, 1, 0, 0);
  capture.Capture(world, 12, hooks);
  CHECK_EQ(stream.Build(capture, nullptr, /*full=*/false, &snap), 1u);
  CHECK_EQ(snap.entities[0].net_id, 1u);

  // Destroyed: despawn rides every delta until a keyframe clears the backlog.
  world.Destroy(a);
  capture.Capture(world, 13, hooks);
  stream.Build(capture, nullptr, /*full=*/false, &snap);
  CHECK(snap.despawned.size() == 1 && snap.despawned[0] == 1u);
  capture.Capture(world, 14, hooks);
  stream.Build(capture, nullptr, /*full=*/false, &snap);
  CHECK(snap.despawned.size() == 1 && snap.despawned[0] == 1u);
  capture.Capture(world, 15, hooks);
  stream.Build(capture, nullptr, /*full=*/true, &snap);
  capture.Capture(world, 16, hooks);
  stream.Build(capture, nullptr, /*full=*/false, &snap);
  CHECK(snap.despawned.empty());
}

void TestPeerStreamInterest() {
  ecs::World world;
  Spawn(world, 1, 0, 0, 0);
  Spawn(world, 2, 100, 0, 0);

  net::ReplicationHooks hooks;
  net::WorldCapture capture;
  net::PeerStream near_stream;
  net::PeerStream far_stream;
  net::Snapshot snap;

  net::InterestSet near_set;
  near_set.Insert(1);
  net::InterestSet far_set;
  far_set.Insert(2);

  capture.Capture(world, 20, hooks);
  // Each stream sees only its slice of the same capture.
  CHECK_EQ(near_stream.Build(capture, &near_set, /*full=*/true, &snap), 1u);
  CHECK_EQ(snap.entities[0].net_id, 1u);
  CHECK_EQ(far_stream.Build(capture, &far_set, /*full=*/true, &snap), 1u);
  CHECK_EQ(snap.entities[0].net_id, 2u);

  // Entity 1 leaves the near set: despawns for that peer only, even though
  // the entity is alive in the world.
  near_set.Clear();
  capture.Capture(world, 21, hooks);
  near_stream.Build(capture, &near_set, /*full=*/false, &snap);
  CHECK(snap.despawned.size() == 1 && snap.despawned[0] == 1u);
  far_stream.Build(capture, &far_set, /*full=*/false, &snap);
  CHECK(snap.despawned.empty());

  // Re-entering the set replays the entity as a fresh spawn record.
  near_set.Insert(1);
  capture.Capture(world, 22, hooks);
  CHECK_EQ(near_stream.Build(capture, &near_set, /*full=*/false, &snap), 1u);
  CHECK_EQ(snap.entities[0].net_id, 1u);
}

// --- snapshot application ---

void TestSnapshotApplier() {
  ecs::World world;
  net::SnapshotApplier applier;

  u64 tagged_entity_tag = 0;
  net::ReplicationHooks hooks;
  hooks.on_replica_spawned = [&](ecs::World&, ecs::Entity, u64 tag) {
    tagged_entity_tag = tag;
  };

  net::Snapshot snap;
  snap.server_tick = 5;
  snap.full = true;
  net::EntityState e = MakeState(1, 1, 2, 3);
  e.user_tag = 0x77;
  snap.entities.push_back(e);
  CHECK(applier.Apply(world, snap, 0.15f, hooks));
  CHECK_EQ(applier.entity_count(), 1u);
  CHECK_EQ(tagged_entity_tag, 0x77u);
  ecs::Entity replica = applier.Find(1);
  CHECK(world.IsAlive(replica));
  CHECK(world.Get<net::InterpolatedTransform>(replica) != nullptr);

  // Stale ticks are dropped.
  snap.server_tick = 4;
  CHECK(!applier.Apply(world, snap, 0.15f, hooks));

  // A keyframe without the entity reconciles it away.
  snap.server_tick = 6;
  snap.full = true;
  snap.entities.clear();
  CHECK(applier.Apply(world, snap, 0.15f, hooks));
  CHECK_EQ(applier.entity_count(), 0u);
  CHECK(!world.IsAlive(replica));
}

void TestInterpolation() {
  ecs::World world;
  ecs::Entity e = world.Create();
  scene::Transform t;
  world.Add(e, t);
  scene::Transform from;
  scene::Transform to;
  to.position[0] = 10;
  world.Add(e, net::InterpolatedTransform{from, to, 0, 1.0f});
  world.Add(e, net::ReplicatedGait{});

  net::TickInterpolation(world, 0.5f);
  const scene::Transform* now = world.Get<scene::Transform>(e);
  CHECK(std::fabs(now->position[0] - 5.0f) < 1e-4f);
  const net::ReplicatedGait* gait = world.Get<net::ReplicatedGait>(e);
  CHECK(gait->moving && std::fabs(gait->speed - 10.0f) < 1e-3f);

  net::TickInterpolation(world, 0.6f);
  now = world.Get<scene::Transform>(e);
  CHECK_EQ(now->position[0], 10.0f);
}

// --- streaming bubbles ---

void TestBubbleMembership() {
  ecs::World world;
  // Peer 1's avatar with a 10-unit bubble at the origin.
  ecs::Entity avatar = Spawn(world, 100, 0, 0, 0);
  world.Add(avatar, net::InterestBubble{1, 10.0f});

  ecs::Entity inside = Spawn(world, 5, 5, 0, 0);
  Spawn(world, 6, 50, 0, 0);  // far outside

  net::InterestMap map;
  net::InterestConfig config;
  config.hysteresis = 1.5f;  // exit at 15
  map.Configure(config);

  map.Update(world, 1);
  const net::InterestSet* set = map.InterestOf(1);
  CHECK(set != nullptr);
  CHECK(set->Contains(100));  // own avatar always relevant
  CHECK(set->Contains(5));
  CHECK(!set->Contains(6));
  CHECK_EQ(map.bubbles().size(), 1u);
  CHECK_EQ(map.bubbles()[0].entity_count, 2u);

  // Hysteresis: between enter (10) and exit (15) an existing member stays...
  MoveTo(world, inside, 12, 0, 0);
  map.Update(world, 2);
  CHECK(map.InterestOf(1)->Contains(5));
  // ...but leaves past the exit radius...
  MoveTo(world, inside, 16, 0, 0);
  map.Update(world, 3);
  CHECK(!map.InterestOf(1)->Contains(5));
  // ...and does NOT re-enter in the hysteresis band from outside.
  MoveTo(world, inside, 12, 0, 0);
  map.Update(world, 4);
  CHECK(!map.InterestOf(1)->Contains(5));
  MoveTo(world, inside, 9, 0, 0);
  map.Update(world, 5);
  CHECK(map.InterestOf(1)->Contains(5));
}

void TestBubbleOwnership() {
  ecs::World world;
  ecs::Entity avatar1 = Spawn(world, 100, 0, 0, 0);
  world.Add(avatar1, net::InterestBubble{1, 10.0f});
  ecs::Entity avatar2 = Spawn(world, 200, 30, 0, 0);
  world.Add(avatar2, net::InterestBubble{2, 10.0f});

  ecs::Entity npc = Spawn(world, 7, 4, 0, 0);  // inside bubble 1 only

  net::InterestMap map;
  map.Configure(net::InterestConfig{});

  int handoffs = 0;
  u32 last_new_owner = 0;
  map.SetOwnerChangedSink([&](u64 net_id, u32, u32 new_peer) {
    if (net_id == 7) {
      ++handoffs;
      last_new_owner = new_peer;
    }
  });

  map.Update(world, 1);
  CHECK_EQ(map.OwnerOf(7), 1u);
  CHECK_EQ(map.OwnerOf(100), 1u);  // avatars belong to their own peer
  CHECK_EQ(map.OwnerOf(200), 2u);
  CHECK_EQ(handoffs, 1);

  // Bubble 2 moves in so both cover the npc: ownership is sticky with 1,
  // even though peer 2 is now closer.
  MoveTo(world, avatar2, 6, 0, 0);
  map.Update(world, 2);
  CHECK_EQ(map.OwnerOf(7), 1u);
  CHECK_EQ(handoffs, 1);

  // Peer 1 walks away; the npc leaves its bubble and hands off to peer 2.
  MoveTo(world, avatar1, 100, 0, 0);
  map.Update(world, 3);
  CHECK_EQ(map.OwnerOf(7), 2u);
  CHECK_EQ(handoffs, 2);
  CHECK_EQ(last_new_owner, 2u);

  // Peer 2 leaves too: back to the server (owner 0).
  MoveTo(world, avatar2, 100, 0, 50);
  map.Update(world, 4);
  CHECK_EQ(map.OwnerOf(7), net::kNoPeer);

  // Tie-break: both bubbles at the same distance claim simultaneously; the
  // lower peer id wins, deterministically.
  MoveTo(world, avatar1, -4, 0, 0);
  MoveTo(world, avatar2, 12, 0, 0);
  map.Update(world, 5);
  CHECK_EQ(map.OwnerOf(7), 1u);

  // A departing peer releases everything it owns.
  map.RemovePeer(1);
  CHECK_EQ(map.OwnerOf(7), net::kNoPeer);
}

void TestBubbleCounts() {
  ecs::World world;
  ecs::Entity avatar1 = Spawn(world, 100, 0, 0, 0);
  world.Add(avatar1, net::InterestBubble{1, 10.0f});
  ecs::Entity avatar2 = Spawn(world, 200, 8, 0, 0);
  world.Add(avatar2, net::InterestBubble{2, 10.0f});
  Spawn(world, 7, 4, 0, 0);  // covered by both bubbles

  net::InterestMap map;
  map.Configure(net::InterestConfig{});
  map.Update(world, 1);

  // Both peers see all three entities (overlapping bubbles)...
  CHECK_EQ(map.InterestOf(1)->size(), 3u);
  CHECK_EQ(map.InterestOf(2)->size(), 3u);
  // ...but the shared npc has exactly one owner.
  const u32 owner = map.OwnerOf(7);
  CHECK(owner == 1 || owner == 2);
  u32 total_owned = 0;
  for (const net::BubbleState& b : map.bubbles()) total_owned += b.owned_count;
  CHECK_EQ(total_owned, 3u);  // two avatars + one npc, each owned once

  CHECK(net::PeerColor(1) != net::PeerColor(2));
}

}  // namespace

int main() {
  TestWireRoundtrip();
  TestProtocolRoundtrip();
  TestPeerStreamDelta();
  TestPeerStreamInterest();
  TestSnapshotApplier();
  TestInterpolation();
  TestBubbleMembership();
  TestBubbleOwnership();
  TestBubbleCounts();
  if (g_failures == 0) {
    std::printf("net_test: all passed\n");
    return 0;
  }
  std::printf("net_test: %d failure(s)\n", g_failures);
  return 1;
}
