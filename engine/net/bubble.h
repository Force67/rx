#ifndef RX_NET_BUBBLE_H_
#define RX_NET_BUBBLE_H_

#include <functional>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"
#include "ecs/world.h"
#include "net/protocol.h"
#include "net/replication.h"
#include "scene/components.h"

namespace rx::net {

// A streaming bubble: the sphere of the world one connected player receives.
// Attached to the player's avatar entity (the bubble follows its Transform).
// Entities carrying a NetworkId replicate to a client only while they are
// inside that client's bubble, so per-client bandwidth scales with local
// density instead of world population -- the lever that lets one server carry
// more players.
struct InterestBubble {
  u32 peer = kNoPeer;  // the transport peer this bubble streams to
  f32 radius = 0;      // world units; entities inside are relevant
};

struct InterestConfig {
  // Entities enter a bubble at `radius` and leave at radius * hysteresis, so
  // one straddling the boundary doesn't spawn/despawn every tick.
  f32 hysteresis = 1.15f;
  // XZ grid cell edge for the broad phase. 0 sizes it from the largest
  // bubble's exit radius each update.
  f32 cell_size = 0;
};

// Computed each server tick from the world's bubbles: per-peer interest sets
// for the snapshot streams, plus a single owner per replicated entity.
//
// Ownership is what keeps overlapping bubbles from tangling: when two
// players' bubbles cover the same entity, exactly one peer owns it --
// deterministically -- and keeps it until its own bubble no longer contains
// the entity (sticky, so ownership doesn't ping-pong on the overlap seam).
// Only then does the entity hand off to the nearest containing bubble (ties
// break to the lower peer id), or back to the server (owner kNoPeer) when no
// bubble holds it. A player's own avatar is always owned by its peer. The
// game decides what ownership gates -- interaction rights, simulation
// islands, update priority -- through OwnerOf and the changed-sink.
class RX_NET_EXPORT InterestMap {
 public:
  void Configure(const InterestConfig& config) { config_ = config; }

  // Recomputes bubbles, membership and ownership from the world. Call once
  // per server tick, before building the peer streams.
  void Update(ecs::World& world, u64 tick);

  // The interest set for one peer, or null when that peer has no bubble.
  const InterestSet* InterestOf(u32 peer) const;

  // The peer owning a replicated entity, kNoPeer = server-owned / unowned.
  u32 OwnerOf(u64 net_id) const;

  // Every bubble as of the last Update, ready to replicate (kBubbleSync) and
  // to visualize.
  const base::Vector<BubbleState>& bubbles() const { return bubbles_; }

  // Forgets a departed peer's bubble state and releases everything it owned.
  void RemovePeer(u32 peer);

  // Invoked from Update for every ownership handoff (old_peer/new_peer are
  // kNoPeer for the server side).
  void SetOwnerChangedSink(std::function<void(u64 net_id, u32 old_peer, u32 new_peer)> sink) {
    owner_changed_ = std::move(sink);
  }

 private:
  struct PeerData {
    InterestSet interest;
  };
  struct Ownership {
    u32 peer = 0;
    u64 since_tick = 0;
  };

  InterestConfig config_;
  base::UnorderedMap<u32, PeerData> peers_;
  base::UnorderedMap<u64, Ownership> owners_;
  base::Vector<BubbleState> bubbles_;
  std::function<void(u64, u32, u32)> owner_changed_;

  // Scratch, kept across updates for capacity.
  struct BubbleRef {
    u32 peer = 0;
    u32 state_index = 0;  // into bubbles_
    f32 center[3] = {0, 0, 0};
    f32 enter_sq = 0;
    f32 exit_sq = 0;
    u64 avatar_net_id = 0;  // the bubble's own entity, always owned by peer
  };
  base::Vector<BubbleRef> scratch_bubbles_;
  base::UnorderedMap<u64, base::Vector<u32>> scratch_grid_;  // cell -> bubble indices
  base::Vector<u64> scratch_removed_;
};

// A stable debug color for a peer's bubble (golden-angle hue walk, packed
// 0xRRGGBB). Shared by the visualizer and any HUD/tint consumer so peer N is
// the same color everywhere.
RX_NET_EXPORT u32 PeerColor(u32 peer);

}  // namespace rx::net

#endif  // RX_NET_BUBBLE_H_
