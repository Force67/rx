#include "world/world_stream.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include <base/check.h>

#include "edit/reflect.h"
#include "scene/components.h"

namespace rx::world {
namespace {

constexpr u32 kMaximumRetainedErrors = 32;

// How many times a cell's payload may fail before the streamer stops offering
// it. A cook error is deterministic: re-reading the same broken bytes every
// retry interval produces the same failure and nothing else, forever.
constexpr u32 kMaximumLoadAttempts = 3;

// How long a suppressed cell waits before it is offered once more. Long enough
// that a genuinely broken cook costs one read every half a minute rather than
// one every retry interval, short enough that a world holed by a momentary
// archive problem heals itself.
constexpr u64 kSuppressedRetryTicks = 1800;

// Shutdown only. Everywhere else teardown is budgeted, because destroying a
// whole cell's rows in one frame is the same hitch as creating them in one.
constexpr u32 kUnbudgeted = ~u32{0};

// Which payload a region is, folded into the region's identity so that a cell
// changing tier is a change the planner sees and schedules like any other. The
// domain owns bits [0, kDomainCount); the tier sits above it.
constexpr u32 TierChannel(Tier tier) { return 1u << (kDomainCount + static_cast<u32>(tier)); }

Tier TierFromChannels(u32 channels) {
  for (u32 tier = 0; tier < kTierCount; ++tier) {
    if (channels & TierChannel(static_cast<Tier>(tier))) return static_cast<Tier>(tier);
  }
  return Tier::kAbsent;
}

const ResidentInstance* FindSorted(const base::Vector<ResidentInstance>& instances, u64 stable_id) {
  auto it = std::lower_bound(
      instances.begin(), instances.end(), stable_id,
      [](const ResidentInstance& instance, u64 wanted) { return instance.stable_id < wanted; });
  return it != instances.end() && it->stable_id == stable_id ? it : nullptr;
}

class ArchiveCellLoader final : public CellLoader {
 public:
  ArchiveCellLoader(const WorldMap& map, const asset::Vfs& vfs) : map_(map), vfs_(vfs) {}

  void Begin(const CellLoadRequest& request) override {
    CellLoadResult result;
    result.ticket = request.ticket;
    result.cell = request.cell;
    result.domain = request.domain;
    result.tier = request.tier;
    result.ok = map_.ReadPayload(vfs_, request.cell, request.domain, request.tier, &result.payload,
                                &result.error);
    if (!result.ok) result.payload = WorldCellPayload{};
    ready_.push_back(std::move(result));
  }

  void Cancel(scene::WorldStreamTicket ticket) override {
    for (size_t i = 0; i < ready_.size();) {
      if (ready_[i].ticket == ticket) {
        ready_.erase(ready_.begin() + i);
      } else {
        ++i;
      }
    }
  }

  void Poll(base::Vector<CellLoadResult>* out) override {
    if (!out) return;
    for (CellLoadResult& result : ready_) out->push_back(std::move(result));
    ready_.clear();
  }

 private:
  const WorldMap& map_;
  const asset::Vfs& vfs_;
  base::Vector<CellLoadResult> ready_;
};

}  // namespace

base::UniquePointer<CellLoader> MakeArchiveCellLoader(const WorldMap& map, const asset::Vfs& vfs) {
  return base::MakeUnique<ArchiveCellLoader>(map, vfs);
}

bool RuntimeComponentLayout(std::string_view component, u32* stride, u64* layout_hash) {
  const edit::ComponentDesc* desc = edit::FindComponentByName(component);
  if (!desc) return false;
  const ecs::ComponentInfo& info = ecs::GetComponentInfo(desc->id);
  base::Vector<std::string_view> names;
  base::Vector<u32> types;
  base::Vector<u32> offsets;
  names.reserve(desc->prop_count);
  types.reserve(desc->prop_count);
  offsets.reserve(desc->prop_count);
  for (u32 i = 0; i < desc->prop_count; ++i) {
    names.push_back(desc->props[i].name);
    types.push_back(static_cast<u32>(desc->props[i].type));
    offsets.push_back(desc->props[i].offset);
  }
  if (stride) *stride = info.size;
  if (layout_hash) {
    *layout_hash = HashComponentLayout(
        component, info.size, std::span<const std::string_view>(names.data(), names.size()),
        std::span<const u32>(types.data(), types.size()),
        std::span<const u32>(offsets.data(), offsets.size()));
  }
  return true;
}

WorldStreamer::WorldStreamer(const WorldMap& map, CellLoader& loader, ecs::World& world)
    : map_(map), loader_(loader), world_(world) {}

WorldStreamer::~WorldStreamer() { Shutdown(); }

void WorldStreamer::Configure(const WorldStreamPolicy& policy) { policy_ = policy; }

bool WorldStreamer::SetOverlay(const WorldOverlay* overlay) {
  if (!overlay) {
    overlay_ = nullptr;
    return true;
  }
  // A bake id of zero means the overlay was never keyed to a world, which is
  // the caller's business. A different one is not: stable ids are assigned by
  // cook order, so applying it here would delete and move whatever rows now
  // happen to carry those ids.
  if (overlay->bake_id() != 0 && overlay->bake_id() != map_.index().bake_id) {
    RecordError("overlay was recorded against bake " + std::to_string(overlay->bake_id()) +
                ", this world is bake " + std::to_string(map_.index().bake_id));
    overlay_ = nullptr;
    return false;
  }
  overlay_ = overlay;
  return true;
}

WorldStreamer::DomainCell* WorldStreamer::Find(DomainState& state, u64 cell) {
  auto it = std::lower_bound(state.cells.begin(), state.cells.end(), cell,
                             [](const DomainCell& entry, u64 wanted) { return entry.cell < wanted; });
  return it != state.cells.end() && it->cell == cell ? it : nullptr;
}

const WorldStreamer::DomainCell* WorldStreamer::Find(const DomainState& state, u64 cell) const {
  auto it = std::lower_bound(state.cells.begin(), state.cells.end(), cell,
                             [](const DomainCell& entry, u64 wanted) { return entry.cell < wanted; });
  return it != state.cells.end() && it->cell == cell ? it : nullptr;
}

WorldStreamer::DomainCell& WorldStreamer::Emplace(DomainState& state, u64 cell) {
  auto it = std::lower_bound(state.cells.begin(), state.cells.end(), cell,
                             [](const DomainCell& entry, u64 wanted) { return entry.cell < wanted; });
  if (it != state.cells.end() && it->cell == cell) return *it;
  DomainCell created;
  created.cell = cell;
  return *state.cells.insert(it, std::move(created));
}

void WorldStreamer::Erase(DomainState& state, u64 cell) {
  if (DomainCell* entry = Find(state, cell)) state.cells.erase(entry);
}

void WorldStreamer::RecordError(std::string message) {
  ++error_count_;
  if (errors_.size() >= kMaximumRetainedErrors) return;
  errors_.push_back(std::move(message));
}

std::span<const std::string> WorldStreamer::errors() const {
  return std::span<const std::string>(errors_.data(), errors_.size());
}

bool WorldStreamer::ResolveSchema(DomainCell& cell, std::string* error) const {
  cell.resolved.clear();
  const WorldCellPayload& payload = cell.payload;
  if (payload.kind == PayloadKind::kInstances) return true;

  const ecs::ComponentId resident_id = ecs::GetComponentId<CellResident>();
  for (u32 a = 0; a < payload.archetypes.size(); ++a) {
    const WorldArchetypeRecord& archetype = payload.archetypes[a];
    if (payload.StableIds(archetype).size() != archetype.row_count) {
      *error = "archetype " + std::to_string(a) + " has no readable stable-id array";
      return false;
    }
    ResolvedArchetype resolved;
    resolved.signature.push_back(resident_id);
    resolved.column_ids.reserve(archetype.column_count);
    for (u32 c = 0; c < archetype.column_count; ++c) {
      const WorldColumnRecord& column = payload.columns[archetype.column_first + c];
      const std::string_view name = payload.String(column.name);
      if (payload.ColumnBytes(column).size() != column.data_bytes) {
        *error = "archetype " + std::to_string(a) + " column '" + std::string(name) +
                 "' is not readable";
        return false;
      }
      const edit::ComponentDesc* desc = edit::FindComponentByName(name);
      if (!desc) {
        *error = "component '" + std::string(name) +
                 "' is not registered in this build; the cook and the runtime disagree about what "
                 "a world may contain";
        return false;
      }
      if (desc->id == resident_id) {
        *error = "payload declares CellResident, which the streamer owns";
        return false;
      }
      const ecs::ComponentInfo& info = ecs::GetComponentInfo(desc->id);
      if (!info.trivially_copyable) {
        *error = "component '" + std::string(name) +
                 "' holds an indirection and cannot be restored by copying bytes";
        return false;
      }
      if (info.size != column.stride) {
        *error = "component '" + std::string(name) + "' is " + std::to_string(info.size) +
                 " bytes here, " + std::to_string(column.stride) + " in the bake";
        return false;
      }
      u64 runtime_hash = 0;
      if (!RuntimeComponentLayout(name, nullptr, &runtime_hash) ||
          runtime_hash != column.layout_hash) {
        *error = "component '" + std::string(name) +
                 "' has a different field layout here than it had at bake time";
        return false;
      }
      for (ecs::ComponentId existing : resolved.column_ids) {
        if (existing != desc->id) continue;
        *error = "archetype " + std::to_string(a) + " lists component '" + std::string(name) +
                 "' twice";
        return false;
      }
      if (desc->id == ecs::GetComponentId<scene::Transform>()) {
        resolved.transform_column = static_cast<int>(resolved.column_ids.size());
      }
      resolved.signature.push_back(desc->id);
      resolved.column_ids.push_back(desc->id);
    }
    std::sort(resolved.signature.begin(), resolved.signature.end());
    cell.resolved.push_back(std::move(resolved));
  }
  return true;
}

bool WorldStreamer::MaterializeStep(DomainCell& cell, u32 rows) {
  const WorldCellPayload& payload = cell.payload;
  // Only a cell the overlay actually mentions pays for consulting it.
  const bool overlaid = overlay_ != nullptr && cell.overlay_touched;

  if (payload.kind == PayloadKind::kInstances) {
    const u32 total = static_cast<u32>(payload.instances.size());
    const u32 take = std::min(rows, total - cell.next_instance);
    for (u32 i = 0; i < take; ++i) {
      const WorldInstanceRecord& source = payload.instances[cell.next_instance + i];
      if (overlaid && overlay_->IsDestroyed(source.stable_id)) continue;
      ResidentInstance instance{source.stable_id, source.prototype, source.position,
                                source.rotation, source.scale};
      if (overlaid) {
        if (const OverlayMove* move = overlay_->FindMove(source.stable_id)) {
          instance.position = move->position;
          instance.rotation = move->rotation;
          instance.scale = move->scale;
        }
      }
      cell.instances.push_back(instance);
    }
    cell.next_instance += take;
    return cell.next_instance >= total;
  }

  const ecs::ComponentId resident_id = ecs::GetComponentId<CellResident>();
  u32 budget = rows;
  while (cell.next_archetype < payload.archetypes.size() && budget > 0) {
    const WorldArchetypeRecord& archetype = payload.archetypes[cell.next_archetype];
    const u32 remaining = archetype.row_count - cell.next_row;
    if (remaining == 0) {
      ++cell.next_archetype;
      cell.next_row = 0;
      continue;
    }
    const ResolvedArchetype& resolved = cell.resolved[cell.next_archetype];
    const std::span<const u64> stable_ids = payload.StableIds(archetype);
    const u32 first = cell.next_row;
    const u32 take = std::min(remaining, budget);
    const u64 owning_cell = cell.cell;

    // A destroyed row is skipped here rather than created and destroyed after
    // the fact: the deletion is never briefly observable, and the row's bytes
    // are never copied at all.
    u32 kept = take;
    if (overlaid) {
      rows_scratch_.clear();
      for (u32 r = first; r < first + take; ++r) {
        if (!overlay_->IsDestroyed(stable_ids[r])) rows_scratch_.push_back(r);
      }
      kept = static_cast<u32>(rows_scratch_.size());
    }

    if (kept != 0) {
      world_.CreateBatch(resolved.signature, kept, [&](const ecs::EntityBatch& batch) {
        auto source_row = [&](u32 i) { return overlaid ? rows_scratch_[i] : first + i; };
        for (u32 c = 0; c < archetype.column_count; ++c) {
          const WorldColumnRecord& column = payload.columns[archetype.column_first + c];
          const std::span<const u8> bytes = payload.ColumnBytes(column);
          const ecs::ComponentId id = resolved.column_ids[c];
          if (!overlaid) {
            const u8* source = bytes.data() + static_cast<size_t>(first) * column.stride;
            u32 written = 0;
            while (written < kept) {
              u32 run = 0;
              void* destination = batch.Column(id, written, &run);
              // ResolveSchema put every column id into the signature this batch
              // was created from, so a missing run would mean the batch is not
              // the archetype we asked for.
              BASE_BUGCHECK(destination != nullptr && run != 0, "world batch column vanished");
              std::memcpy(destination, source + static_cast<size_t>(written) * column.stride,
                          static_cast<size_t>(run) * column.stride);
              written += run;
            }
            continue;
          }
          for (u32 i = 0; i < kept; ++i) {
            u32 run = 0;
            void* destination = batch.Column(id, i, &run);
            BASE_BUGCHECK(destination != nullptr && run != 0, "world batch column vanished");
            std::memcpy(destination,
                        bytes.data() + static_cast<size_t>(source_row(i)) * column.stride,
                        column.stride);
          }
        }
        u32 written = 0;
        while (written < kept) {
          u32 run = 0;
          auto* residents = static_cast<CellResident*>(batch.Column(resident_id, written, &run));
          BASE_BUGCHECK(residents != nullptr && run != 0, "world batch resident column vanished");
          for (u32 i = 0; i < run; ++i) {
            new (residents + i) CellResident{stable_ids[source_row(written + i)], owning_cell};
          }
          written += run;
        }
        if (overlaid && resolved.transform_column >= 0) {
          const ecs::ComponentId transform_id = resolved.column_ids[resolved.transform_column];
          for (u32 i = 0; i < kept; ++i) {
            const OverlayMove* move = overlay_->FindMove(stable_ids[source_row(i)]);
            if (!move) continue;
            u32 run = 0;
            auto* transform = static_cast<scene::Transform*>(batch.Column(transform_id, i, &run));
            BASE_BUGCHECK(transform != nullptr && run != 0, "world batch transform vanished");
            transform->position[0] = move->position.x;
            transform->position[1] = move->position.y;
            transform->position[2] = move->position.z;
            transform->rotation[0] = move->rotation.x;
            transform->rotation[1] = move->rotation.y;
            transform->rotation[2] = move->rotation.z;
            transform->rotation[3] = move->rotation.w;
            transform->scale = move->scale;
          }
        }
        for (u32 i = 0; i < kept; ++i) {
          cell.entities.push_back({stable_ids[source_row(i)], batch.EntityAt(i)});
        }
      });
    }

    cell.next_row += take;
    budget -= take;
  }
  // Step past archetypes the last quantum finished exactly, so a completed cell
  // never needs one more empty commit to notice it is done.
  while (cell.next_archetype < payload.archetypes.size() &&
         cell.next_row >= payload.archetypes[cell.next_archetype].row_count) {
    ++cell.next_archetype;
    cell.next_row = 0;
  }
  return cell.next_archetype >= payload.archetypes.size();
}

bool WorldStreamer::TeardownStep(DomainCell& cell, u32 rows) {
  u32 budget = rows;
  while (!cell.entities.empty() && budget > 0) {
    world_.Destroy(cell.entities.back().entity);
    cell.entities.pop_back();
    --budget;
  }
  if (!cell.entities.empty()) return false;
  cell.instances.clear();
  cell.payload = WorldCellPayload{};
  cell.resolved.clear();
  cell.next_archetype = 0;
  cell.next_row = 0;
  cell.next_instance = 0;
  cell.overlay_touched = false;
  cell.resident_bytes = 0;
  return true;
}

void WorldStreamer::DrainLoader() {
  results_scratch_.clear();
  loader_.Poll(&results_scratch_);
  for (CellLoadResult& result : results_scratch_) {
    // The domain comes back from a loader implementation this class does not
    // own, so it indexes an array only after it has been checked.
    if (static_cast<u32>(result.domain) >= kDomainCount) {
      RecordError("a cell loader returned an unknown domain for cell " +
                  std::to_string(result.cell));
      continue;
    }
    DomainState& state = domains_[static_cast<u32>(result.domain)];
    DomainCell* cell = Find(state, result.cell);
    // A result for a generation that no longer owns the cell is not a result at
    // all: the region was cancelled and re-prepared while it was in flight.
    if (!cell || !(cell->ticket == result.ticket) || cell->phase != CellPhase::kLoading) continue;

    // A read failure already names the path it failed on; a schema failure is
    // about the payload's contents and does not.
    std::string error;
    if (result.ok) {
      cell->payload = std::move(result.payload);
      if (!ResolveSchema(*cell, &error)) {
        result.ok = false;
        error = CellPayloadPath(map_.payload_prefix(), result.cell, result.domain, result.tier) +
                ": " + error;
      }
    } else {
      error = std::move(result.error);
    }

    if (!result.ok) {
      NoteLoadFailure(state, result.cell);
      RecordError(std::move(error));
      cell->payload = WorldCellPayload{};
      cell->resolved.clear();
      // Out of kLoading, so a loader that delivers the same ticket twice cannot
      // be counted as two failures.
      cell->phase = CellPhase::kFailed;
      scene::ApplyWorldStreamPrepareResult(state.plan, cell->ticket,
                                           scene::WorldStreamPrepareResult::kFailed);
      continue;
    }
    if (!scene::ApplyWorldStreamPrepareResult(state.plan, cell->ticket,
                                              scene::WorldStreamPrepareResult::kReady)) {
      // The plan rejected the ticket after all. Drop the bytes rather than let
      // a later commit adopt them, and drop the record too: leaving it behind
      // would keep counting as pending with nothing on the way.
      Erase(state, cell->cell);
      continue;
    }
    // A load that succeeds clears the cell's tally: only a run of failures
    // should latch, and a transient one followed by a read is not that.
    auto failed = std::lower_bound(
        state.failed.begin(), state.failed.end(), cell->cell,
        [](const FailedCell& entry, u64 wanted) { return entry.cell < wanted; });
    if (failed != state.failed.end() && failed->cell == cell->cell) state.failed.erase(failed);

    const WorldCellRecord* record = map_.index().FindCell(cell->cell);
    cell->overlay_touched =
        overlay_ != nullptr && record != nullptr &&
        overlay_->TouchesRange(record->stable_id_first, record->stable_id_count);
    cell->phase = CellPhase::kDecoded;
  }
  results_scratch_.clear();
}

void WorldStreamer::AdvanceRetirements(Domain domain, DomainState& state) {
  const u32 rows = std::max(1u, policy_[domain].rows_per_commit);
  for (size_t i = 0; i < state.cells.size();) {
    DomainCell& cell = state.cells[i];
    if (cell.phase != CellPhase::kRetiring) {
      ++i;
      continue;
    }
    if (!TeardownStep(cell, rows)) {
      ++i;
      continue;
    }
    const scene::WorldStreamTicket ticket = cell.ticket;
    state.cells.erase(state.cells.begin() + i);
    scene::ApplyWorldStreamRetireResult(state.plan, ticket);
  }
}

void WorldStreamer::GatherClaims(Domain domain, DomainState& state) {
  if (!claims_ || claims_->empty()) return;
  const u32 channel = 1u << static_cast<u32>(domain);
  for (const ClaimEntry& entry : claims_->entries()) {
    if ((entry.claim.domains & channel) == 0 || !claims_->Honors(entry.claim)) continue;
    const WorldCellRecord* record = map_.index().FindCell(entry.claim.cell);
    if (!record || !map_.HasDomain(*record, domain)) continue;

    // A claim is a streaming source standing at the middle of one cell with no
    // radius at all. Distance to its own bounds is zero, so the cell loads;
    // distance to any other cell is positive, so nothing else does.
    scene::WorldStreamObservation observation;
    observation.position = {(record->minimum.x + record->maximum.x) * 0.5f,
                            (record->minimum.y + record->maximum.y) * 0.5f,
                            (record->minimum.z + record->maximum.z) * 0.5f};
    observation.load_distance = 0;
    observation.retain_distance = 0;
    observation.channels = channel;
    observation.axes = scene::kWorldStreamXYZ;
    state.observations.push_back(observation);

    // Nested cells can contain the claimed cell's centre; a claim contributes
    // exactly the cell it names and nothing that happens to surround it.
    claim_scratch_.clear();
    map_.GatherRegions(observation, domain, &claim_scratch_);
    for (const CellDemand& demand : claim_scratch_) {
      if (demand.region.id != entry.claim.cell) continue;
      CellDemand claimed = demand;
      // A claim that does not ask for detail must not decide the band: its
      // source stands at the cell's own middle, so its distance is zero and it
      // would pin the near tier for as long as the lease lived. One that does
      // ask counts exactly like an observer standing there, which is what it is.
      claimed.from_claim = !entry.claim.full_detail;
      state.demands.push_back(claimed);
    }
  }
}

void WorldStreamer::MergeCandidates(Domain domain, DomainState& state) {
  const DomainStreamPolicy& domain_policy = policy_[domain];
  const u32 channel = 1u << static_cast<u32>(domain);

  std::sort(state.demands.begin(), state.demands.end(),
            [](const CellDemand& a, const CellDemand& b) {
              if (a.region.id != b.region.id) return a.region.id < b.region.id;
              return a.distance < b.distance;
            });

  state.candidates.clear();
  state.bands_scratch.clear();
  for (size_t i = 0; i < state.demands.size();) {
    // Sorted by (id, distance), so the first entry of a run is the nearest
    // observer's view of that cell.
    const CellDemand& nearest = state.demands[i];
    const u64 id = nearest.region.id;
    size_t end = i;
    while (end < state.demands.size() && state.demands[end].region.id == id) ++end;
    // The band follows whoever is actually near the cell. A claim's source
    // stands at the cell's own middle, so letting it into this would mean that
    // taking or dropping a lease evicts and rebuilds a cell that was already
    // resident and correct, in both directions.
    f32 band_distance = std::numeric_limits<f32>::infinity();
    for (size_t d = i; d < end; ++d) {
      if (state.demands[d].from_claim) continue;
      band_distance = std::min(band_distance, state.demands[d].distance);
    }
    i = end;

    if (Suppressed(state, id)) continue;

    const WorldCellRecord* record = map_.index().FindCell(id);
    if (!record) continue;

    auto previous = std::lower_bound(
        state.bands.begin(), state.bands.end(), id,
        [](const CellBand& entry, u64 wanted) { return entry.cell < wanted; });
    const bool was_near =
        previous != state.bands.end() && previous->cell == id && previous->near;
    const bool near = InNearTierBand(domain_policy, band_distance, was_near);
    const Tier tier = ResolveTier(map_.index(), *record, domain,
                                  near ? domain_policy.near_tier : domain_policy.far_tier);
    if (tier == Tier::kAbsent) continue;
    state.bands_scratch.push_back({id, near});

    scene::WorldStreamRegion region = nearest.region;
    // A world baked at one tier resolves both bands to the same payload, so its
    // channels never change and crossing the band costs nothing.
    region.channels = channel | TierChannel(tier);
    // Correctness is admitted before quality: a hard claim's cell outranks a
    // nearer cell that only the picture depends on.
    if (claims_) region.priority = std::max(region.priority, claims_->Priority(id, channel));
    state.candidates.push_back(region);
  }
  state.bands = state.bands_scratch;
}

void WorldStreamer::NoteLoadFailure(DomainState& state, u64 cell) {
  auto it = std::lower_bound(state.failed.begin(), state.failed.end(), cell,
                             [](const FailedCell& entry, u64 wanted) { return entry.cell < wanted; });
  if (it == state.failed.end() || it->cell != cell) {
    it = state.failed.insert(it, FailedCell{cell, 0, 0});
  }
  ++it->attempts;
  if (it->attempts >= kMaximumLoadAttempts) it->retry_at_tick = tick_ + kSuppressedRetryTicks;
}

bool WorldStreamer::Suppressed(const DomainState& state, u64 cell) const {
  auto it = std::lower_bound(state.failed.begin(), state.failed.end(), cell,
                             [](const FailedCell& entry, u64 wanted) { return entry.cell < wanted; });
  if (it == state.failed.end() || it->cell != cell) return false;
  return it->attempts >= kMaximumLoadAttempts && tick_ < it->retry_at_tick;
}

void WorldStreamer::ClearFailures() {
  for (u32 i = 0; i < kDomainCount; ++i) domains_[i].failed.clear();
}

void WorldStreamer::UpdateDomain(Domain domain,
                                 std::span<const scene::WorldStreamObservation> observers) {
  DomainState& state = domains_[static_cast<u32>(domain)];
  const DomainStreamPolicy& domain_policy = policy_[domain];

  state.observations.clear();
  state.demands.clear();
  for (const scene::WorldStreamObservation& observer : observers) {
    scene::WorldStreamObservation narrowed;
    if (!MakeDomainObservation(observer, domain, policy_, &narrowed)) continue;
    state.observations.push_back(narrowed);
    map_.GatherRegions(narrowed, domain, &state.demands);
  }
  GatherClaims(domain, state);
  MergeCandidates(domain, state);

  AdvanceRetirements(domain, state);

  scene::AdvanceWorldStreaming(
      state.plan, std::span<const scene::WorldStreamObservation>(state.observations.data(),
                                                                 state.observations.size()),
      std::span<const scene::WorldStreamRegion>(state.candidates.data(), state.candidates.size()),
      domain_policy.budget, &state.actions);

  for (const scene::WorldStreamAction& action : state.actions) {
    switch (action.kind) {
      case scene::WorldStreamActionKind::kPrepare: {
        // Which payload was decided in MergeCandidates and carried here in the
        // region's channels, so the tier that was scheduled is the tier that is
        // read - the distance in the action is only a priority.
        const WorldCellRecord* record = map_.index().FindCell(action.region.id);
        const Tier tier = TierFromChannels(action.region.channels);
        if (!record || tier == Tier::kAbsent) {
          RecordError("cell " + std::to_string(action.region.id) + " " + DomainName(domain) +
                      ": prepared with no tier the index knows");
          scene::ApplyWorldStreamPrepareResult(state.plan, action.ticket,
                                               scene::WorldStreamPrepareResult::kFailed);
          break;
        }
        DomainCell& cell = Emplace(state, action.region.id);
        // A prepare only ever follows a completed retirement, but Emplace may
        // hand back a record an earlier generation left behind, so every field
        // is set rather than only the ones that usually differ.
        cell.ticket = action.ticket;
        cell.tier = tier;
        cell.phase = CellPhase::kLoading;
        cell.payload = WorldCellPayload{};
        cell.resolved.clear();
        cell.next_archetype = 0;
        cell.next_row = 0;
        cell.next_instance = 0;
        cell.overlay_touched = false;
        cell.entities.clear();
        cell.instances.clear();
        const WorldPayloadRecord* payload = map_.index().FindPayload(*record, domain, tier);
        cell.resident_bytes = payload ? payload->resident_bytes : 0;
        loader_.Begin({action.ticket, cell.cell, domain, tier});
        break;
      }
      case scene::WorldStreamActionKind::kCommit: {
        DomainCell* cell = Find(state, action.ticket.region);
        if (!cell || !(cell->ticket == action.ticket) || cell->phase != CellPhase::kDecoded) {
          // Unreachable: the plan only commits what it saw prepared. Recorded
          // rather than swallowed so that if it ever does happen, the message
          // is the thing that says so.
          RecordError("cell " + std::to_string(action.ticket.region) + " " + DomainName(domain) +
                      ": commit arrived for a cell that is not decoded");
          scene::ApplyWorldStreamCommitResult(state.plan, action.ticket,
                                              scene::WorldStreamCommitResult::kFailed);
          break;
        }
        if (!MaterializeStep(*cell, std::max(1u, domain_policy.rows_per_commit))) {
          scene::ApplyWorldStreamCommitResult(state.plan, action.ticket,
                                              scene::WorldStreamCommitResult::kMoreWork);
          break;
        }
        std::sort(cell->entities.begin(), cell->entities.end(),
                  [](const StableEntity& a, const StableEntity& b) {
                    return a.stable_id < b.stable_id;
                  });
        std::sort(cell->instances.begin(), cell->instances.end(),
                  [](const ResidentInstance& a, const ResidentInstance& b) {
                    return a.stable_id < b.stable_id;
                  });
        cell->phase = CellPhase::kPublished;
        // The payload's bytes have served their purpose; the ECS and the
        // instance page own the state now.
        cell->payload = WorldCellPayload{};
        cell->resolved.clear();
        scene::ApplyWorldStreamCommitResult(state.plan, action.ticket,
                                            scene::WorldStreamCommitResult::kComplete);
        break;
      }
      case scene::WorldStreamActionKind::kCancel:
      case scene::WorldStreamActionKind::kUnload: {
        if (action.kind == scene::WorldStreamActionKind::kCancel) loader_.Cancel(action.ticket);
        DomainCell* cell = Find(state, action.ticket.region);
        if (!cell || !(cell->ticket == action.ticket)) {
          // Nothing of ours under that ticket: acknowledge so the plan can let
          // the region go.
          scene::ApplyWorldStreamRetireResult(state.plan, action.ticket);
          break;
        }
        // Teardown is budgeted, cancel and unload alike, and a cell parked in
        // kFailed by a refused read comes through here too. The planner will not
        // admit a new generation for a region it still tracks, and it tracks a
        // retiring one until ApplyWorldStreamRetireResult, so there is nothing
        // to be gained by destroying a large half-built cell in one frame - on
        // the cancel path least of all, since that is the observer moving fast.
        cell->phase = CellPhase::kRetiring;
        break;
      }
    }
  }
}

void WorldStreamer::Update(std::span<const scene::WorldStreamObservation> observers) {
  if (shut_down_) return;
  ++tick_;
  DrainLoader();
  for (u32 i = 0; i < kDomainCount; ++i) UpdateDomain(static_cast<Domain>(i), observers);
}

void WorldStreamer::Shutdown() {
  if (shut_down_) return;
  shut_down_ = true;
  base::Vector<scene::WorldStreamAction> actions;
  for (u32 i = 0; i < kDomainCount; ++i) {
    DomainState& state = domains_[i];
    scene::ResetWorldStreaming(state.plan, &actions);
    for (const scene::WorldStreamAction& action : actions) {
      if (action.kind == scene::WorldStreamActionKind::kCancel) loader_.Cancel(action.ticket);
      scene::ApplyWorldStreamRetireResult(state.plan, action.ticket);
    }
    // Unbudgeted: the caller is about to destroy the ecs::World, so every row
    // this streamer created has to be gone before this returns.
    for (DomainCell& cell : state.cells) TeardownStep(cell, kUnbudgeted);
    state.cells.clear();
    // A region that was already retiring when Shutdown ran gets no action from
    // Reset and so is never acknowledged, which leaves the plan non-empty and
    // still flagged as resetting. That is deliberate: the plan owns nothing but
    // its own vectors, and Update never advances it again.
  }
}

ecs::Entity WorldStreamer::Resolve(u64 stable_id) const {
  const WorldCellRecord* record = map_.index().FindCellByStableId(stable_id);
  if (!record) return {};
  for (u32 i = 0; i < kDomainCount; ++i) {
    const DomainCell* cell = Find(domains_[i], record->id);
    // A cell that is still materializing, or already dying, has no answer to
    // give: half its rows exist and the other half never will.
    if (!cell || cell->phase != CellPhase::kPublished) continue;
    auto it = std::lower_bound(
        cell->entities.begin(), cell->entities.end(), stable_id,
        [](const StableEntity& entry, u64 wanted) { return entry.stable_id < wanted; });
    // A game that destroys a cell's entity itself leaves the record behind
    // until the cell unloads, so the handle is checked rather than trusted.
    if (it != cell->entities.end() && it->stable_id == stable_id &&
        world_.IsAlive(it->entity)) {
      return it->entity;
    }
  }
  return {};
}

std::span<const ResidentInstance> WorldStreamer::Instances(u64 cell_id, Domain domain) const {
  if (static_cast<u32>(domain) >= kDomainCount) return {};
  const DomainCell* cell = Find(domains_[static_cast<u32>(domain)], cell_id);
  if (!cell || cell->phase != CellPhase::kPublished) return {};
  return std::span<const ResidentInstance>(cell->instances.data(), cell->instances.size());
}

const ResidentInstance* WorldStreamer::FindInstance(u64 stable_id) const {
  const WorldCellRecord* record = map_.index().FindCellByStableId(stable_id);
  if (!record) return nullptr;
  for (u32 i = 0; i < kDomainCount; ++i) {
    const DomainCell* cell = Find(domains_[i], record->id);
    if (!cell || cell->phase != CellPhase::kPublished) continue;
    if (const ResidentInstance* found = FindSorted(cell->instances, stable_id)) return found;
  }
  return nullptr;
}

ecs::Entity WorldStreamer::Promote(u64 stable_id) {
  const WorldCellRecord* record = map_.index().FindCellByStableId(stable_id);
  if (!record) return {};
  for (u32 i = 0; i < kDomainCount; ++i) {
    DomainState& state = domains_[i];
    DomainCell* cell = Find(state, record->id);
    if (!cell || cell->phase != CellPhase::kPublished) continue;
    const ResidentInstance* instance = FindSorted(cell->instances, stable_id);
    if (!instance) continue;
    auto slot = std::lower_bound(
        cell->entities.begin(), cell->entities.end(), stable_id,
        [](const StableEntity& entry, u64 wanted) { return entry.stable_id < wanted; });
    if (slot != cell->entities.end() && slot->stable_id == stable_id) return {};  // already promoted

    // The entity gets an identity and a place in the world; what it means -
    // which mesh, which behavior - is the caller's to add from the prototype.
    const ecs::Entity entity = world_.Create();
    scene::Transform transform;
    transform.position[0] = instance->position.x;
    transform.position[1] = instance->position.y;
    transform.position[2] = instance->position.z;
    transform.rotation[0] = instance->rotation.x;
    transform.rotation[1] = instance->rotation.y;
    transform.rotation[2] = instance->rotation.z;
    transform.rotation[3] = instance->rotation.w;
    transform.scale = instance->scale;
    world_.Add(entity, transform);
    world_.Add(entity, CellResident{stable_id, cell->cell});
    cell->entities.insert(slot, StableEntity{stable_id, entity});
    return entity;
  }
  return {};
}

WorldStreamerStats WorldStreamer::stats() const {
  WorldStreamerStats stats;
  for (u32 i = 0; i < kDomainCount; ++i) {
    const DomainState& state = domains_[i];
    for (const DomainCell& cell : state.cells) {
      switch (cell.phase) {
        case CellPhase::kPublished:
          ++stats.resident;
          stats.resident_bytes += cell.resident_bytes;
          break;
        case CellPhase::kRetiring:
          // Its rows still exist and still cost, which is exactly when a memory
          // budget reading this must not be told they are already gone.
          stats.resident_bytes += cell.resident_bytes;
          ++stats.pending;
          break;
        case CellPhase::kLoading:
        case CellPhase::kDecoded:
        case CellPhase::kFailed:
          ++stats.pending;
          break;
      }
      stats.entities += static_cast<u32>(cell.entities.size());
      stats.instances += static_cast<u32>(cell.instances.size());
    }
    // From the streamer's own tally rather than the plan's kFailed count: the
    // plan parks a cell there on the way through an ordinary tier change too,
    // and a suppressed cell has left the plan entirely.
    for (const FailedCell& failed : state.failed) {
      if (Suppressed(state, failed.cell)) {
        ++stats.suppressed;
      } else {
        ++stats.failed;
      }
    }
  }
  return stats;
}

}  // namespace rx::world
