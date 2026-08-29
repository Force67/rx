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

  // Reads and decodes one payload. Fails when the entry is missing, corrupt, or
  // describes a different cell, domain, tier or bake than the index promised:
  // a payload that disagrees with the index it was addressed through means the
  // archive and the index came from different cooks.
  bool ReadPayload(const asset::Vfs& vfs, u64 cell, Domain domain, Tier tier,
                   WorldCellPayload* out, std::string* error) const;

  // Every cell the query could reach that has any payload for `domain`, as
  // streaming regions with the cell id as the region id. Conservative: the
  // planner's own bounds test decides load and retain, so a cell emitted here
  // that turns out to be too far is merely wasted comparison, while one missed
  // here would be read as having left the world.
  void GatherRegions(const scene::WorldStreamQuery& query, Domain domain,
                     base::Vector<scene::WorldStreamRegion>* out) const;

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
  // Cells nearer than this take the higher tier. A cell keeps the tier it was
  // prepared with for as long as it stays resident: tiers are alternatives, and
  // swapping one for another under a moving observer is churn, not detail.
  f32 full_tier_distance = 0;
  Tier near_tier = Tier::kFull;
  Tier far_tier = Tier::kProxy;
  // Per-domain budgets, so one domain's payload sizes cannot starve another's.
  scene::WorldStreamFrameBudget budget;
  // Rows materialized per commit quantum. Publishing an entire cell in one
  // frame is how a streaming engine stays inside its memory budget and still
  // hitches.
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

// The tier this domain wants for a cell at `distance`, capped by what the cook
// actually produced. kAbsent means nothing to load.
RX_WORLD_EXPORT Tier TargetTier(const WorldIndexData& index, const WorldCellRecord& cell,
                                Domain domain, const DomainStreamPolicy& policy, f32 distance);

}  // namespace rx::world

#endif  // RX_WORLD_WORLD_MAP_H_
