#include "net/bubble.h"

#include <cmath>

namespace rx::net {
namespace {

// Packs a signed XZ cell coordinate into one grid key.
u64 CellKey(i32 cx, i32 cz) {
  return (static_cast<u64>(static_cast<u32>(cx)) << 32) | static_cast<u32>(cz);
}

f32 DistanceSq(const f32 a[3], const f32 b[3]) {
  f32 sum = 0;
  for (int i = 0; i < 3; ++i) {
    const f32 d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

}  // namespace

void InterestMap::Update(ecs::World& world, u64 tick) {
  // --- gather bubbles ---
  bubbles_.clear();
  scratch_bubbles_.clear();
  f32 max_exit = 0;
  world.Each<InterestBubble, scene::Transform>(
      [&](ecs::Entity entity, InterestBubble& bubble, scene::Transform& t) {
        if (bubble.peer == kNoPeer || bubble.radius <= 0) return;
        BubbleRef ref;
        ref.peer = bubble.peer;
        ref.state_index = static_cast<u32>(bubbles_.size());
        for (int i = 0; i < 3; ++i) ref.center[i] = t.position[i];
        const f32 exit = bubble.radius * (config_.hysteresis > 1.0f ? config_.hysteresis : 1.0f);
        ref.enter_sq = bubble.radius * bubble.radius;
        ref.exit_sq = exit * exit;
        if (exit > max_exit) max_exit = exit;
        if (const NetworkId* id = world.Get<NetworkId>(entity)) {
          ref.avatar_net_id = id->value;
        }
        scratch_bubbles_.push_back(ref);

        BubbleState state;
        state.peer = bubble.peer;
        for (int i = 0; i < 3; ++i) state.center[i] = t.position[i];
        state.radius = bubble.radius;
        bubbles_.push_back(state);
      });

  // Drop peers whose bubble disappeared (avatar destroyed) and start every
  // surviving peer's membership fresh; hysteresis reads the previous set
  // through prev-membership captured below.
  scratch_removed_.clear();
  for (auto entry : peers_) {
    bool alive = false;
    for (const BubbleRef& b : scratch_bubbles_) {
      if (b.peer == entry.key) {
        alive = true;
        break;
      }
    }
    if (!alive) scratch_removed_.push_back(entry.key);
  }
  for (u64 peer : scratch_removed_) RemovePeer(static_cast<u32>(peer));

  if (scratch_bubbles_.empty()) {
    // No bubbles: everything falls back to the server.
    scratch_removed_.clear();
    for (auto owned : owners_) scratch_removed_.push_back(owned.key);
    for (u64 net_id : scratch_removed_) {
      const Ownership* o = owners_.find(net_id);
      if (o && owner_changed_) owner_changed_(net_id, o->peer, kNoPeer);
      owners_.erase(net_id);
    }
    return;
  }

  // --- broad phase: bucket bubbles into the XZ cells their exit sphere overlaps ---
  const f32 cell = config_.cell_size > 0 ? config_.cell_size
                                         : (max_exit > 1.0f ? max_exit : 1.0f);
  const f32 inv_cell = 1.0f / cell;
  scratch_grid_.clear();
  for (u32 bi = 0; bi < scratch_bubbles_.size(); ++bi) {
    const BubbleRef& b = scratch_bubbles_[bi];
    const f32 exit = std::sqrt(b.exit_sq);
    const i32 min_x = static_cast<i32>(std::floor((b.center[0] - exit) * inv_cell));
    const i32 max_x = static_cast<i32>(std::floor((b.center[0] + exit) * inv_cell));
    const i32 min_z = static_cast<i32>(std::floor((b.center[2] - exit) * inv_cell));
    const i32 max_z = static_cast<i32>(std::floor((b.center[2] + exit) * inv_cell));
    for (i32 cx = min_x; cx <= max_x; ++cx) {
      for (i32 cz = min_z; cz <= max_z; ++cz) {
        const u64 key = CellKey(cx, cz);
        base::Vector<u32>* bucket = scratch_grid_.find(key);
        if (!bucket) {
          scratch_grid_.insert(key, base::Vector<u32>{});
          bucket = scratch_grid_.find(key);
        }
        bucket->push_back(bi);
      }
    }
  }

  // Previous membership feeds hysteresis; rebuild each peer's set in place by
  // swapping into a scratch copy first.
  base::UnorderedMap<u32, InterestSet> previous;
  for (auto entry : peers_) {
    InterestSet moved = std::move(entry.value.interest);
    previous.insert(entry.key, std::move(moved));
    entry.value.interest.Clear();
  }
  // Peers appearing for the first time this update.
  for (const BubbleRef& b : scratch_bubbles_) {
    if (!peers_.find(b.peer)) peers_.insert(b.peer, PeerData{});
  }

  // --- membership + ownership, one pass over the replicated entities ---
  // Candidates for one entity land here; bubbles are few per cell so a flat
  // scan beats anything fancier.
  struct Candidate {
    u32 peer;
    f32 dist_sq;
    bool inside_exit;
  };
  base::Vector<Candidate> candidates;
  base::UnorderedMap<u64, u8> seen_ids;

  world.Each<NetworkId, scene::Transform>(
      [&](ecs::Entity, NetworkId& id, scene::Transform& t) {
        if (id.value == 0) return;
        seen_ids.insert(id.value, u8{1});
        candidates.clear();

        const i32 cx = static_cast<i32>(std::floor(t.position[0] * inv_cell));
        const i32 cz = static_cast<i32>(std::floor(t.position[2] * inv_cell));
        const base::Vector<u32>* bucket = scratch_grid_.find(CellKey(cx, cz));
        u32 avatar_peer = kNoPeer;
        if (bucket) {
          for (u32 bi : *bucket) {
            BubbleRef& b = scratch_bubbles_[bi];
            if (b.avatar_net_id == id.value) avatar_peer = b.peer;
            const f32 d_sq = DistanceSq(b.center, t.position);
            if (d_sq > b.exit_sq) continue;
            const InterestSet* prev = nullptr;
            if (const auto* pd = previous.find(b.peer)) prev = pd;
            const bool was_member = prev && prev->Contains(id.value);
            // Enter at the inner radius, stay until the outer one.
            if (d_sq <= b.enter_sq || was_member) {
              candidates.push_back({b.peer, d_sq, true});
              if (PeerData* peer_data = peers_.find(b.peer)) {
                peer_data->interest.Insert(id.value);
              }
              bubbles_[b.state_index].entity_count++;
            }
          }
        }

        // --- ownership ---
        Ownership* owned = owners_.find(id.value);
        const u32 prev_owner = owned ? owned->peer : kNoPeer;
        u32 next_owner = prev_owner;

        if (avatar_peer != kNoPeer) {
          // A player's avatar belongs to its own peer, always.
          next_owner = avatar_peer;
        } else {
          bool prev_owner_holds = false;
          for (const Candidate& c : candidates) {
            if (c.peer == prev_owner && c.inside_exit) prev_owner_holds = true;
          }
          if (!prev_owner_holds) {
            // Handoff: nearest containing bubble, ties to the lower peer id;
            // no bubble means back to the server.
            next_owner = kNoPeer;
            f32 best = 0;
            for (const Candidate& c : candidates) {
              if (next_owner == kNoPeer || c.dist_sq < best ||
                  (c.dist_sq == best && c.peer < next_owner)) {
                next_owner = c.peer;
                best = c.dist_sq;
              }
            }
          }
        }

        if (next_owner != prev_owner) {
          if (next_owner == kNoPeer) {
            owners_.erase(id.value);
          } else if (owned) {
            owned->peer = next_owner;
            owned->since_tick = tick;
          } else {
            owners_.insert(id.value, Ownership{next_owner, tick});
          }
          if (owner_changed_) owner_changed_(id.value, prev_owner, next_owner);
        }
      });

  // Owned entities that vanished from the world release silently.
  scratch_removed_.clear();
  for (auto owned : owners_) {
    if (!seen_ids.find(owned.key)) scratch_removed_.push_back(owned.key);
  }
  for (u64 net_id : scratch_removed_) owners_.erase(net_id);

  // Per-bubble owned counts for the replicated/visualized states.
  for (auto owned : owners_) {
    for (const BubbleRef& b : scratch_bubbles_) {
      if (b.peer == owned.value.peer) {
        bubbles_[b.state_index].owned_count++;
        break;
      }
    }
  }
}

const InterestSet* InterestMap::InterestOf(u32 peer) const {
  const PeerData* data = peers_.find(peer);
  return data ? &data->interest : nullptr;
}

u32 InterestMap::OwnerOf(u64 net_id) const {
  const Ownership* owned = owners_.find(net_id);
  return owned ? owned->peer : kNoPeer;
}

void InterestMap::RemovePeer(u32 peer) {
  peers_.erase(peer);
  scratch_removed_.clear();
  for (auto owned : owners_) {
    if (owned.value.peer == peer) scratch_removed_.push_back(owned.key);
  }
  for (u64 net_id : scratch_removed_) {
    owners_.erase(net_id);
    if (owner_changed_) owner_changed_(net_id, peer, kNoPeer);
  }
}

u32 PeerColor(u32 peer) {
  // Golden-angle hue walk: well-spread, stable, no table.
  const f32 hue = std::fmod(static_cast<f32>(peer) * 137.50776f, 360.0f) / 60.0f;
  const f32 x = 1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f);
  f32 r = 0, g = 0, b = 0;
  switch (static_cast<int>(hue)) {
    case 0: r = 1; g = x; break;
    case 1: r = x; g = 1; break;
    case 2: g = 1; b = x; break;
    case 3: g = x; b = 1; break;
    case 4: r = x; b = 1; break;
    default: r = 1; b = x; break;
  }
  const u32 ri = static_cast<u32>(r * 255.0f + 0.5f);
  const u32 gi = static_cast<u32>(g * 255.0f + 0.5f);
  const u32 bi = static_cast<u32>(b * 255.0f + 0.5f);
  return (ri << 16) | (gi << 8) | bi;
}

}  // namespace rx::net
