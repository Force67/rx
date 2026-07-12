#ifndef RX_NET_REPLICATION_H_
#define RX_NET_REPLICATION_H_

#include <functional>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"
#include "ecs/world.h"
#include "net/protocol.h"
#include "scene/components.h"

namespace rx::net {

// Stable identity across machines. Local Entity values differ per peer, the
// NetworkId is what snapshots address.
struct NetworkId {
  u64 value = 0;
};

// Server-side id source. Ids are never reused within a session; 0 means
// "not replicated".
RX_NET_EXPORT NetworkId AllocateNetworkId();

// Client-side smoothing for remotely simulated transforms. Snapshots arrive
// at a fraction of the render rate, so the visible transform blends from the
// state at the previous snapshot to the newest one over the snapshot
// interval.
struct InterpolatedTransform {
  scene::Transform from;
  scene::Transform to;
  f32 elapsed = 0;
  f32 duration = 0;  // 0 snaps
};

// Render gait derived for a replicated actor. Snapshots carry transforms, not
// animation state, so a walking actor would otherwise slide across a client
// without stepping. TickInterpolation fills this from the planar velocity of
// the segment it is blending across; the game reads it to drive the actor's
// gait. Planar means the XZ ground plane (engine up is +Y), matching the speed
// the host feeds its own locomotion.
struct ReplicatedGait {
  f32 speed = 0;        // units per second along the ground plane
  bool moving = false;  // speed above a small idle threshold
};

// Advances every InterpolatedTransform and writes the blended result into the
// entity's Transform. Runs on clients in the pre-render stage. Where an
// entity also carries ReplicatedGait, fills it from the planar velocity of
// the active interpolation segment so the game can animate walking actors.
RX_NET_EXPORT void TickInterpolation(ecs::World& world, f32 dt);

// The game's per-entity payload seam. The engine replicates transform + mesh;
// everything else a game addresses entities by travels in the opaque
// EntityState::user_tag through these hooks. Unset hooks mean "no tag".
struct ReplicationHooks {
  // Server capture: the tag stored for an entity (0 = none) -- e.g. a game
  // packs its form/record id here.
  std::function<u64(ecs::World&, ecs::Entity)> capture_user_tag;
  // Client apply: called once when a replica spawns with a non-zero tag, so
  // the game attaches whatever component the tag denotes.
  std::function<void(ecs::World&, ecs::Entity, u64 user_tag)> on_replica_spawned;
};

// Set of net ids relevant to one peer. Bubble membership feeds this; a
// PeerStream treats anything outside the set exactly like a destroyed entity,
// which is what makes bubbles a bandwidth cut rather than a special case.
struct InterestSet {
  void Insert(u64 net_id) { ids_.insert(net_id, u8{1}); }
  bool Contains(u64 net_id) const { return ids_.find(net_id) != nullptr; }
  void Clear() { ids_.clear(); }
  size_t size() const { return ids_.size(); }

 private:
  base::UnorderedMap<u64, u8> ids_;
};

// One pass over the world per server tick, shared by every peer's stream:
// walks entities carrying NetworkId + Transform out of the archetype storage
// and captures their EntityState records once, so per-peer work is pure
// delta bookkeeping.
class RX_NET_EXPORT WorldCapture {
 public:
  void Capture(ecs::World& world, u64 server_tick, const ReplicationHooks& hooks);

  const base::Vector<EntityState>& entities() const { return entities_; }
  u64 tick() const { return tick_; }

 private:
  base::Vector<EntityState> entities_;
  u64 tick_ = 0;
};

// Per-client delta stream over a shared WorldCapture. Delta snapshots carry
// only entities whose state changed since this stream's previous build; full
// snapshots (keyframes) carry everything relevant and let the client
// reconcile despawns it may have missed on the unreliable channel. An entity
// that leaves the stream's interest set despawns on that client just like a
// destroyed one, freeing the client's memory and this stream's cache.
class RX_NET_EXPORT PeerStream {
 public:
  // Fills `out` from the capture. `interest` limits the stream to the ids it
  // contains; null replicates everything (interest management off). Returns
  // the number of entity records written. `out` is reused by the caller
  // across ticks so the vectors keep their capacity.
  u32 Build(const WorldCapture& capture, const InterestSet* interest, bool full,
            Snapshot* out);

 private:
  struct Cached {
    EntityState state;
    u64 seen_tick = 0;
  };

  base::UnorderedMap<u64, Cached> sent_;
  // Despawns accumulate until the next full snapshot covers them, since any
  // single delta carrying them can be lost.
  base::Vector<u64> pending_despawns_;
  base::Vector<u64> scratch_removed_;
};

// Client side: maps net ids onto local entities, spawning and despawning as
// snapshots dictate, and feeds InterpolatedTransform instead of writing
// transforms directly.
class RX_NET_EXPORT SnapshotApplier {
 public:
  // Applies a decoded snapshot. `lerp_duration` is the expected gap between
  // snapshots. Returns false when the snapshot is stale (out-of-order
  // delivery). `hooks.on_replica_spawned` fires for new replicas carrying a
  // user tag.
  bool Apply(ecs::World& world, const Snapshot& snapshot, f32 lerp_duration,
             const ReplicationHooks& hooks);

  // Destroys every entity this applier spawned. Used on disconnect.
  void Reset(ecs::World& world);

  u64 latest_tick() const { return latest_tick_; }
  ecs::Entity Find(u64 net_id) const;
  u32 entity_count() const { return static_cast<u32>(entities_.size()); }

 private:
  struct Replica {
    ecs::Entity entity;
    u64 seen_tick = 0;
  };

  base::UnorderedMap<u64, Replica> entities_;
  base::Vector<u64> scratch_removed_;
  u64 latest_tick_ = 0;
};

}  // namespace rx::net

#endif  // RX_NET_REPLICATION_H_
