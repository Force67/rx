#include "world/world_map.h"

#include <algorithm>
#include <cmath>

namespace rx::world {
namespace {

void SetError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

std::string DirectoryOf(std::string_view path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return {};
  return std::string(path.substr(0, slash));
}

}  // namespace

bool WorldMap::Load(const asset::Vfs& vfs, std::string_view index_path, std::string* error) {
  loaded_ = false;
  index_ = WorldIndexData{};
  payload_prefix_.clear();
  index_path_.assign(index_path);

  std::optional<base::Vector<u8>> bytes = vfs.Read(index_path);
  if (!bytes) {
    SetError(error, std::string(index_path) + ": no such world index in the mounted archives");
    return false;
  }
  std::string decode_error;
  if (!DecodeWorldIndex(std::span<const u8>(bytes->data(), bytes->size()), &index_,
                        &decode_error)) {
    SetError(error, std::string(index_path) + ": " + decode_error);
    index_ = WorldIndexData{};
    return false;
  }
  payload_prefix_ = DirectoryOf(index_path);
  loaded_ = true;
  return true;
}

bool WorldMap::ReadPayload(const asset::Vfs& vfs, u64 cell, Domain domain, Tier tier,
                           WorldCellPayload* out, std::string* error) const {
  if (!out) return false;
  const std::string path = CellPayloadPath(payload_prefix_, cell, domain, tier);
  std::optional<base::Vector<u8>> bytes = vfs.Read(path);
  if (!bytes) {
    SetError(error, path + ": the index lists this payload, but the archive has no such entry");
    return false;
  }
  std::string decode_error;
  if (!DecodeCellPayload(std::span<const u8>(bytes->data(), bytes->size()), out, &decode_error)) {
    SetError(error, path + ": " + decode_error);
    return false;
  }
  // A payload that disagrees with the index it was reached through is not a
  // payload to be fixed up: it means the index and the archive came from
  // different cooks, and every other cell is suspect too.
  if (out->bake_id != index_.bake_id) {
    SetError(error, path + ": baked by " + std::to_string(out->bake_id) + ", but " + index_path_ +
                        " was baked by " + std::to_string(index_.bake_id));
    return false;
  }
  if (out->cell_id != cell || out->domain != domain || out->tier != tier) {
    SetError(error, path + ": holds cell " + std::to_string(out->cell_id) + " " +
                        DomainName(out->domain) + "/" + TierName(out->tier) + ", not cell " +
                        std::to_string(cell) + " " + DomainName(domain) + "/" + TierName(tier));
    return false;
  }
  return true;
}

bool WorldMap::HasDomain(const WorldCellRecord& cell, Domain domain) const {
  for (u32 i = 0; i < cell.payload_count; ++i) {
    if (index_.payloads[cell.payload_first + i].domain == domain) return true;
  }
  return false;
}

void WorldMap::GatherRegions(const scene::WorldStreamObservation& observer, Domain domain,
                             base::Vector<CellDemand>* out) const {
  if (!out || !loaded_) return;
  const u32 channel = 1u << static_cast<u32>(domain);
  if ((observer.channels & channel) == 0) return;
  for (const WorldCellRecord& cell : index_.cells) {
    if (!HasDomain(cell, domain)) continue;
    const scene::WorldStreamRegion region{cell.id, cell.minimum, cell.maximum, 0, channel};
    const scene::WorldStreamDemand demand = scene::EvaluateWorldStreamDemand(observer, region);
    if (!demand.retain) continue;
    out->push_back(
        {region, std::min(demand.current_distance, demand.predicted_distance), demand.load});
  }
}

WorldStreamPolicy DefaultWorldStreamPolicy(f32 scale) {
  const f32 unit = std::isfinite(scale) && scale > 0 ? scale : 1.0f;
  WorldStreamPolicy policy;
  auto set = [&](Domain domain, f32 load, f32 retain, f32 full, u32 rows_per_commit) {
    DomainStreamPolicy& target = policy[domain];
    target.load_distance = load * unit;
    target.retain_distance = retain * unit;
    target.full_tier_distance = full * unit;
    target.rows_per_commit = rows_per_commit;
  };
  // Gameplay and collision are what correctness depends on, so they load first
  // and at the shortest radius; representation reaches further because a hole
  // in the picture is more visible than a hole in the simulation is felt.
  set(Domain::kGameplay, 128, 192, 96, 512);
  set(Domain::kCollision, 128, 192, 96, 2048);
  set(Domain::kRepresentation, 256, 320, 128, 8192);
  set(Domain::kNavigation, 384, 448, 192, 1024);
  set(Domain::kLighting, 256, 320, 128, 1024);
  set(Domain::kAudio, 0, 0, 0, 1024);  // opt in per game

  // Swapping tiers is a reload: the old tier's rows are destroyed before the
  // new tier's arrive. For anything the simulation depends on that reads as an
  // entity despawning and respawning as the player walks up, so the domains
  // that carry behavior stay on one tier by default and only the ones whose
  // gap is a moment of coarser scenery band by distance. Set near_tier apart
  // from far_tier per game once the cook produces a tier worth the swap.
  for (Domain domain : {Domain::kGameplay, Domain::kCollision, Domain::kNavigation}) {
    policy[domain].near_tier = Tier::kFull;
    policy[domain].far_tier = Tier::kFull;
  }
  return policy;
}

bool MakeDomainObservation(const scene::WorldStreamObservation& observer, Domain domain,
                           const WorldStreamPolicy& policy,
                           scene::WorldStreamObservation* out) {
  if (!out) return false;
  const DomainStreamPolicy& domain_policy = policy[domain];
  if (!(domain_policy.load_distance > 0)) return false;
  const u32 channel = 1u << static_cast<u32>(domain);
  if ((observer.channels & channel) == 0) return false;
  *out = observer;
  out->load_distance = domain_policy.load_distance;
  out->retain_distance = std::max(domain_policy.load_distance, domain_policy.retain_distance);
  out->channels = channel;
  return true;
}

bool InNearTierBand(const DomainStreamPolicy& policy, f32 distance, bool currently_near) {
  const f32 margin = policy.tier_hysteresis > 1.0f ? policy.tier_hysteresis : 1.0f;
  const f32 threshold =
      currently_near ? policy.full_tier_distance * margin : policy.full_tier_distance;
  return std::isfinite(distance) && distance <= threshold;
}

Tier ResolveTier(const WorldIndexData& index, const WorldCellRecord& cell, Domain domain,
                 Tier wanted) {
  const Tier best = index.BestTier(cell, domain, wanted);
  if (best != Tier::kAbsent) return best;
  // Payloads are sorted by tier within a domain, so the first is the cheapest.
  for (u32 i = 0; i < cell.payload_count; ++i) {
    const WorldPayloadRecord& payload = index.payloads[cell.payload_first + i];
    if (payload.domain == domain) return payload.tier;
  }
  return Tier::kAbsent;
}

Tier TargetTier(const WorldIndexData& index, const WorldCellRecord& cell, Domain domain,
                const DomainStreamPolicy& policy, f32 distance) {
  const bool near = InNearTierBand(policy, distance, /*currently_near=*/false);
  return ResolveTier(index, cell, domain, near ? policy.near_tier : policy.far_tier);
}

}  // namespace rx::world
