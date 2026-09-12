#ifndef RX_WORLD_WORLD_MAP_H_
#define RX_WORLD_WORLD_MAP_H_

#include <span>
#include <string>
#include <string_view>

#include <base/containers/vector.h>

#include "asset/vfs.h"
#include "core/export.h"
#include "scene/world_streaming.h"
#include "world/world_format.h"

namespace rx::world {

// One cell one observer wants, and how far away the planner thinks it is: the
// nearer of the current and swept-predicted distances, which is the number the
// planner itself schedules by. The streamer bands tiers on it, so a tier
// decision and an admission decision can never be made from different numbers.
struct CellDemand {
  scene::WorldStreamRegion region;
  f32 distance = 0;
  // A claim's synthetic source stands at the middle of the cell it names, so
  // its distance is always zero. That is what admits the cell; it is not
  // evidence about detail, and the tier band ignores it.
  bool from_claim = false;
};

// A loaded world: the index, resident for the lifetime of the world, plus the
// archive it was mounted from. Everything a streaming decision needs is here
// and in memory; reading a payload is only ever the consequence of a decision,
// never part of making one.
//
// The index and its payloads normally live in the same .rxp, mounted into the
// Vfs like any other content, which means a mod archive mounted later can
// override individual cells for free.
class RX_WORLD_EXPORT WorldMap {
 public:
  // `index_path` is a virtual path ("world://city/city.rxworld"). Payloads are
  // read from the same directory, under the CellPayloadPath convention.
  bool Load(const asset::Vfs& vfs, std::string_view index_path, std::string* error);

  bool loaded() const { return loaded_; }
  const WorldIndexData& index() const { return index_; }
  const std::string& payload_prefix() const { return payload_prefix_; }

  // Reads and decodes one payload. Fails when the entry is missing, corrupt,
  // describes a different cell, domain, tier or bake than the index promised,
  // or carries a stable id outside the range the index gives that cell: a
  // payload that disagrees with the index it was addressed through means the
  // archive and the index came from different cooks.
  bool ReadPayload(const asset::Vfs& vfs, u64 cell, Domain domain, Tier tier,
                   WorldCellPayload* out, std::string* error) const;

  // Every cell this observer retains that has any payload for `domain`.
  //
  // The test is the planner's own EvaluateWorldStreamDemand rather than a
  // cheaper approximation of it, because the two must agree exactly. A cell the
  // planner would still retain but this pass omits is not merely a missed
  // optimization: AdvanceWorldStreaming reads an absent candidate as the cell
  // having left the world, and retires it on the spot, ignoring the retain
  // radius that exists to prevent precisely that.
  //
  // This walks every cell. A spatial index belongs in front of it once a world
  // is large enough to want one, as a broad phase whose survivors still take
  // this exact test.
  void GatherRegions(const scene::WorldStreamObservation& observer, Domain domain,
                     base::Vector<CellDemand>* out) const;

  // Whether the cook produced anything at all for this domain.
  bool HasDomain(const WorldCellRecord& cell, Domain domain) const;

 private:
  WorldIndexData index_;
  std::string payload_prefix_;
  std::string index_path_;
  bool loaded_ = false;
};

// How far each domain streams. Domains are independent on purpose: a headless
// server can carry gameplay, collision and navigation at full radius and never
// touch lighting or audio, and a spectator client the reverse, without either
// being a different kind of cell.
struct DomainStreamPolicy {
  f32 load_distance = 0;    // 0 disables the domain outright
  f32 retain_distance = 0;  // hysteresis; clamped up to load_distance
  // Cells nearer than this take near_tier, the rest far_tier. Tiers are
  // alternatives rather than increments, so crossing the band is a reload: the
  // streamer folds the band into the region's identity and the planner runs its
  // ordinary budgeted retire-and-re-prepare inside the retain radius. Setting
  // near_tier == far_tier turns the whole mechanism off.
  f32 full_tier_distance = 0;
  // How far past full_tier_distance a cell already at near_tier is allowed to
  // drift before dropping back. Without it a cell sitting on the boundary
  // reloads every time the observer breathes.
  f32 tier_hysteresis = 1.25f;
  Tier near_tier = Tier::kFull;
  Tier far_tier = Tier::kProxy;
  // Per-domain budgets, so one domain's payload sizes cannot starve another's.
  scene::WorldStreamFrameBudget budget;
  // Rows materialized per commit quantum, and destroyed per teardown quantum.
  // Publishing an entire cell in one frame is how a streaming engine stays
  // inside its memory budget and still hitches. Note the asymmetry: the planner
  // admits at most maximum_commit_steps cells per tick, but every retiring cell
  // gets a quantum, so a tick can destroy more rows than it creates.
  u32 rows_per_commit = 512;
};

struct WorldStreamPolicy {
  DomainStreamPolicy domains[kDomainCount];

  const DomainStreamPolicy& operator[](Domain domain) const {
    return domains[static_cast<u32>(domain)];
  }
  DomainStreamPolicy& operator[](Domain domain) { return domains[static_cast<u32>(domain)]; }
};

// A starting point: gameplay and collision close in, representation further
// out, navigation and lighting wider still, audio off. Tune per game; the
// point is that the numbers are per domain rather than one radius for a cell.
RX_WORLD_EXPORT WorldStreamPolicy DefaultWorldStreamPolicy(f32 scale);

// Narrows an observer to one domain: its radii come from the policy, not from
// the observer, so the same player position produces a different bubble per
// domain. Returns false when the domain is disabled.
RX_WORLD_EXPORT bool MakeDomainObservation(const scene::WorldStreamObservation& observer,
                                           Domain domain, const WorldStreamPolicy& policy,
                                           scene::WorldStreamObservation* out);

// Whether a cell at `distance` sits in the near band. `currently_near` is the
// band it is already in, so the hysteresis margin only applies on the way out.
RX_WORLD_EXPORT bool InNearTierBand(const DomainStreamPolicy& policy, f32 distance,
                                    bool currently_near);

// The best tier at or below `wanted` that the cook actually produced. When it
// produced nothing that cheap, the cheapest it did produce: a proxy is a worse
// answer than the full thing and a much better one than a hole. kAbsent means
// the cook produced nothing for this domain at all.
RX_WORLD_EXPORT Tier ResolveTier(const WorldIndexData& index, const WorldCellRecord& cell,
                                 Domain domain, Tier wanted);

// The two together: what this domain wants for a cell at `distance`, capped by
// what exists. Equivalent to ResolveTier(TierBand(...)) with no hysteresis.
RX_WORLD_EXPORT Tier TargetTier(const WorldIndexData& index, const WorldCellRecord& cell,
                                Domain domain, const DomainStreamPolicy& policy, f32 distance);

}  // namespace rx::world

#endif  // RX_WORLD_WORLD_MAP_H_
