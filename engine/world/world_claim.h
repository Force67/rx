#ifndef RX_WORLD_WORLD_CLAIM_H_
#define RX_WORLD_WORLD_CLAIM_H_

#include <span>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"
#include "world/world_format.h"

namespace rx::world {

// Why a cell is resident when no observer is near it.
//
// The alternative is the ad hoc pin: a script says "keep this loaded", and six
// months later nobody can say who said it, why, or when it stops being true. A
// claim carries all three, so "why is this cell still resident?" has an answer
// (Explain) instead of a guess.
//
// A claim is not a separate mechanism bolted onto streaming. It becomes a
// streaming source pinned to one cell, which is what it always was: a teleport
// destination, a quest that must keep running, an AI route being planned, a
// cutscene about to play. The planner sees it the same way it sees a player.
//
// Two limits worth knowing before relying on it. A claim's only lever over
// scheduling is the region priority, and the planner sorts starvation age ahead
// of priority when it hands out commit quanta, so a hard claim is admitted
// early but does not preempt an older request already waiting. And the set has
// no clock: Expire is the host's to call, from whatever tick it advances. A
// lease nobody expires is exactly the immortal pin this exists to replace.
enum class ClaimKind : u8 {
  // Correctness. Unloading this would be a bug: the player is standing on it,
  // a network-authoritative entity lives in it, a cutscene actor is in it.
  kHard = 0,
  // Quality. Worth keeping, safe to drop: predicted camera movement, a likely
  // encounter, an audio prefetch.
  kSoft = 1,
  // A guess. The first thing to give up.
  kSpeculative = 2,
};

RX_WORLD_EXPORT const char* ClaimKindName(ClaimKind kind);

struct ResidencyClaim {
  u64 owner = 0;  // whoever asked: a quest id, a script handle, a peer
  u64 cell = 0;
  u32 domains = ~u32{0};  // bit per Domain; see DomainMask
  ClaimKind kind = ClaimKind::kSoft;
  // The tick this claim stops counting, or 0 for "until it is removed". A lease
  // rather than a pin: the failure mode of a pin is that nobody releases it.
  u64 expires_at_tick = 0;
  // A static string naming the reason, for the debugger and the log. Nothing
  // parses it; it exists so a resident cell can say why.
  const char* reason = "";
};

constexpr u32 DomainMask(Domain domain) { return 1u << static_cast<u32>(domain); }

struct ClaimEntry {
  u64 handle = 0;
  ResidencyClaim claim;
};

class RX_WORLD_EXPORT ClaimSet {
 public:
  // Returns a handle to release the claim with. Handles are never reused.
  u64 Add(const ResidencyClaim& claim);
  bool Remove(u64 handle);
  // Every claim one owner holds, for a system tearing itself down.
  size_t RemoveOwner(u64 owner);
  // Drops leases whose deadline has passed. The host advances the tick; the
  // set has no clock of its own, so this stays deterministic.
  size_t Expire(u64 tick);
  void Clear();

  // Under memory pressure the host raises the bar and everything weaker stops
  // contributing. Hard claims are never revocable - that is the whole content
  // of the distinction - so raising it past kHard is refused.
  void set_weakest_honored(ClaimKind kind);
  ClaimKind weakest_honored() const { return weakest_honored_; }

  bool Honors(const ResidencyClaim& claim) const { return claim.kind <= weakest_honored_; }

  std::span<const ClaimEntry> entries() const {
    return std::span<const ClaimEntry>(entries_.data(), entries_.size());
  }
  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

  // Whether any honored claim covers this cell and any of these domains.
  bool Holds(u64 cell, u32 domains) const;
  // How far ahead of ordinary demand a claim on this cell should be admitted.
  // Correctness outranks quality; no claim at all outranks nothing.
  i32 Priority(u64 cell, u32 domains) const;
  // Every honored claim keeping this cell resident, most binding first.
  void Explain(u64 cell, u32 domains, base::Vector<ResidencyClaim>* out) const;

 private:
  base::Vector<ClaimEntry> entries_;
  u64 next_handle_ = 1;
  ClaimKind weakest_honored_ = ClaimKind::kSpeculative;
};

}  // namespace rx::world

#endif  // RX_WORLD_WORLD_CLAIM_H_
