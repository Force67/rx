#ifndef RX_WORLD_WORLD_STREAM_H_
#define RX_WORLD_WORLD_STREAM_H_

#include <span>
#include <string>

#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "asset/vfs.h"
#include "core/export.h"
#include "ecs/world.h"
#include "scene/world_streaming.h"
#include "world/world_claim.h"
#include "world/world_map.h"
#include "world/world_overlay.h"

namespace rx::world {

// Carried by every entity a cell materializes. The stable id is the only
// identity that survives an unload: an ecs::Entity handle is reused with a new
// generation as soon as its slot is freed, so nothing outside a resident cell
// may hold one. Anything that must refer across a streaming boundary - a save
// file, a quest, a network peer - stores the stable id and resolves it.
struct CellResident {
  u64 stable_id = 0;
  u64 cell = 0;
};

// A resident static decoration instance. It has a stable world id, so it can be
// picked, deleted by an overlay, or promoted into an entity later, but it costs
// no ECS row until something needs its behavior.
struct ResidentInstance {
  u64 stable_id = 0;
  // What this instance is, as an index into the cell's prototype table
  // (WorldStreamer::Prototypes). The table outlives the payload it was decoded
  // from; the index alone would not, which is the point of keeping it.
  u32 prototype = 0;
  Vec3 position;
  Quat rotation;
  f32 scale = 1.0f;
  // Set once Promote has given this instance an entity. The row stays in the
  // page - nothing removes it - so a renderer drawing the page skips these or
  // draws the same rock twice.
  bool promoted = false;
};

struct CellLoadRequest {
  scene::WorldStreamTicket ticket;
  u64 cell = 0;
  Domain domain = Domain::kGameplay;
  Tier tier = Tier::kAbsent;
};

struct CellLoadResult {
  scene::WorldStreamTicket ticket;
  u64 cell = 0;
  Domain domain = Domain::kGameplay;
  Tier tier = Tier::kAbsent;
  bool ok = false;
  std::string error;
  WorldCellPayload payload;
};

// Everything between "the planner asked for this payload" and "its bytes are
// decoded". Splitting it out is what makes the streamer testable: the archive
// implementation reads through the Vfs, and a test hands back results in
// whatever order it wants to prove the streamer survives.
//
// Begin, Cancel and Poll are all called on the streamer's own thread. An
// implementation that does the work elsewhere owns its own synchronization and
// must tolerate Cancel arriving for a ticket it has already completed.
//
// WorldStreamer::Shutdown does not Poll: an implementation with results still
// in flight owns them, and must be able to drop them (and join its workers)
// when it is destroyed.
class RX_WORLD_EXPORT CellLoader {
 public:
  virtual ~CellLoader() = default;

  virtual void Begin(const CellLoadRequest& request) = 0;
  // Cancels a request Begin was given. It takes the whole request, not just the
  // ticket: every domain runs its own plan and every plan numbers generations
  // from one, so cell 0's first gameplay request and its first representation
  // request carry the identical ticket. A loader keyed on the ticket alone
  // cancels whichever it finds, and the other waits for a result that is never
  // coming.
  virtual void Cancel(const CellLoadRequest& request) = 0;
  // Moves out everything that finished since the last call.
  virtual void Poll(base::Vector<CellLoadResult>* out) = 0;
};

// Reads and decodes through the Vfs. `map` and `vfs` must outlive the loader.
RX_WORLD_EXPORT base::UniquePointer<CellLoader> MakeArchiveCellLoader(const WorldMap& map,
                                                                      const asset::Vfs& vfs);

struct WorldStreamerStats {
  u32 resident = 0;   // (cell, domain) pairs fully published
  u32 pending = 0;    // reading, decoded-not-published, or retiring
  // Cells whose last payload read or schema check was refused and which are
  // waiting to be tried again. A cell merely changing tier is not one of these.
  u32 failed = 0;
  // (cell, domain) pairs the streamer has stopped offering because their
  // payload failed too many times. They are gone from the world without being
  // gone from the index, so a world that is quietly missing a cell says so here
  // rather than only in the capped errors() list.
  u32 suppressed = 0;
  u32 entities = 0;   // ECS rows this streamer owns
  u32 instances = 0;  // static instances resident
  u64 resident_bytes = 0;
  // Every load failure since construction, including the ones past the cap on
  // the retained messages. errors() says which; this says how many.
  u32 errors_total = 0;
};

// Drives one baked world: one streaming plan per domain, each with its own
// radii and budget, materializing cooked payloads into an ecs::World and
// tearing them down again.
//
// Everything happens on the calling thread. The streamer never blocks on I/O
// itself; it asks the CellLoader and picks the results up on a later tick.
class RX_WORLD_EXPORT WorldStreamer {
 public:
  WorldStreamer(const WorldMap& map, CellLoader& loader, ecs::World& world);
  ~WorldStreamer();

  WorldStreamer(const WorldStreamer&) = delete;
  WorldStreamer& operator=(const WorldStreamer&) = delete;

  void Configure(const WorldStreamPolicy& policy);
  const WorldStreamPolicy& policy() const { return policy_; }

  // The sparse deltas applied on top of every cell this streamer materializes.
  // The overlay is the caller's, and must outlive the streamer.
  //
  // It decides what a cell looks like on the way in, and does not reach back
  // into cells that are already resident: a game that destroys something
  // destroys the entity itself and records it here so the deletion survives the
  // next reload. Do not edit it while a cell is mid-commit - the streamer reads
  // it once per quantum, so a cell straddling the edit would come up half from
  // each version of the save.
  //
  // False, with a message in errors() and the overlay left unset, when the
  // overlay was recorded against a different bake than this world. Its stable
  // ids would name different rows, so it would not fail, it would delete and
  // move the wrong things. Passing null clears it and always succeeds.
  bool SetOverlay(const WorldOverlay* overlay);

  // Residency claims held by systems that need a cell whether or not anyone is
  // standing near it. Each honored claim becomes a streaming source pinned to
  // its cell for this tick, so the planner weighs it exactly as it weighs a
  // player. A claim is an explicit request rather than a distance heuristic,
  // so it is honored even for a domain whose radius policy is zero. The set is
  // the caller's, and must outlive the streamer.
  void SetClaims(const ClaimSet* claims) { claims_ = claims; }

  // One tick. `observers` are world-space streaming sources - the player, the
  // camera, a teleport destination, an AI route request; the streamer derives
  // one bubble per domain from each.
  void Update(std::span<const scene::WorldStreamObservation> observers);

  // Retires everything and drains it in this call, so the ecs::World can be
  // destroyed afterwards. Terminal and idempotent: Update does nothing after
  // it, because a streamer is cheap to rebuild and a half-restarted one is not
  // worth the states it would add.
  void Shutdown();

  // The live entity for a stable id, or a null entity when its cell is not
  // resident, is still materializing, is retiring, or the game destroyed that
  // entity itself. Resolution goes through the index's stable-id ranges, so an
  // id belonging to an unloaded cell is answered without any of it resident.
  ecs::Entity Resolve(u64 stable_id) const;

  // Resident static instances of one cell and domain, empty when that cell is
  // not published. Instance pages are per domain, so which one is asked for is
  // the caller's to say.
  //
  // The span is invalidated by the next Update, which may move or free the
  // cell's storage; copy anything that has to outlive the tick.
  std::span<const ResidentInstance> Instances(u64 cell,
                                              Domain domain = Domain::kRepresentation) const;

  // Every cell of a domain that is published right now, in ascending id order.
  // A host draws instance pages by walking this rather than the whole index:
  // the index is the world, and this is the part of it that is here.
  void ResidentCells(Domain domain, base::Vector<u64>* out) const;

  // The names ResidentInstance::prototype indexes: what each instance is, in
  // whatever vocabulary the cook used (a mesh path, a prefab name). Held by the
  // cell for as long as it is resident, because the payload they were decoded
  // from is dropped the moment the cell is published. Same span lifetime as
  // Instances.
  std::span<const std::string> Prototypes(u64 cell,
                                          Domain domain = Domain::kRepresentation) const;
  // The instance carrying `stable_id`, or null.
  const ResidentInstance* FindInstance(u64 stable_id) const;

  // Turns a static instance into a real entity: the point where a rock stops
  // being a row in a page and starts being something with behavior. The
  // instance stays in the page and is marked ResidentInstance::promoted; what
  // it gains is an ECS identity under the same stable id. Nothing removes the
  // page row, so a renderer drawing the page has to skip promoted instances or
  // draw the same rock twice. Returns a null entity when the instance is not
  // resident, or when it was already promoted.
  ecs::Entity Promote(u64 stable_id);

  WorldStreamerStats stats() const;

  // Forgets which cells have been failing, so the next tick offers them again.
  // For a host that has just changed what is mounted: the streamer cannot tell
  // a broken cook from an archive that was missing for a moment, and this is
  // how it is told the difference.
  void ClearFailures();

  // Load failures since construction, oldest first and capped. A refused
  // payload is a cook or archive bug, so the messages are kept rather than
  // counted: knowing which cell and why is the whole value. The span is
  // invalidated by the next failure.
  std::span<const std::string> errors() const;

 private:
  struct StableEntity {
    u64 stable_id = 0;
    ecs::Entity entity;
  };

  struct ResolvedArchetype {
    ecs::Signature signature;
    // Runtime component id per column of this archetype, in payload order.
    base::Vector<ecs::ComponentId> column_ids;
    // Which of those columns is the Transform an overlay move rewrites, or -1.
    int transform_column = -1;
  };

  enum class CellPhase : u8 {
    kLoading,    // asked the loader, nothing decoded yet
    kDecoded,    // payload validated, waiting for commit quanta
    kPublished,  // fully materialized
    kFailed,     // the read or the schema was refused; awaiting the plan's cancel
    kRetiring,   // being torn down, possibly over several ticks
  };

  struct DomainCell {
    u64 cell = 0;
    scene::WorldStreamTicket ticket;
    Tier tier = Tier::kAbsent;
    CellPhase phase = CellPhase::kLoading;
    WorldCellPayload payload;
    base::Vector<ResolvedArchetype> resolved;
    u32 next_archetype = 0;
    u32 next_row = 0;
    u32 next_instance = 0;
    base::Vector<StableEntity> entities;  // sorted by stable id once published
    base::Vector<ResidentInstance> instances;
    // Lifted out of the payload before it is dropped, so a resident instance
    // can still say what it is.
    base::Vector<std::string> prototypes;
    // Whether the overlay has anything to say about this cell's stable-id
    // range. Decided once, when the payload arrives, so an untouched cell takes
    // the bulk copy path rather than the row-by-row one.
    bool overlay_touched = false;
    u64 resident_bytes = 0;
  };

  // Which tier band a cell was last placed in, so the hysteresis margin has
  // something to compare against. Kept apart from residency on purpose: the
  // band is a fact about where the observers are, and it has to survive the
  // window where a cell is retiring at one tier and not yet resident at the
  // other.
  struct CellBand {
    u64 cell = 0;
    bool near = false;
  };

  // A cell whose payload has failed to load. A cook error is deterministic, so
  // retrying it forever re-reads and re-decodes the same broken bytes every
  // retry interval; past a few attempts the cell stops being offered at all.
  //
  // The tally is per cell and domain, not per tier: a cell whose near-tier
  // payload is broken stops being offered at its working far tier too. That is
  // the coarse answer, and the loud one - stats().suppressed counts it.
  //
  // Suppression throttles rather than forbids. The failures this counts are not
  // all deterministic: an archive briefly unmounted, or a read that fails under
  // load, would otherwise put a permanent hole in the world over a stall that
  // lasted a second. A suppressed cell is offered again once, long after, and
  // re-suppressed if it fails again.
  struct FailedCell {
    u64 cell = 0;
    u32 attempts = 0;
    u64 retry_at_tick = 0;
  };

  struct DomainState {
    scene::WorldStreamPlan plan;
    base::Vector<DomainCell> cells;  // sorted by cell id
    base::Vector<scene::WorldStreamObservation> observations;
    base::Vector<CellDemand> demands;
    base::Vector<scene::WorldStreamRegion> candidates;
    base::Vector<scene::WorldStreamAction> actions;
    base::Vector<CellBand> bands;  // sorted by cell id
    base::Vector<CellBand> bands_scratch;
    base::Vector<FailedCell> failed;  // sorted by cell id
  };

  DomainCell* Find(DomainState& state, u64 cell);
  const DomainCell* Find(const DomainState& state, u64 cell) const;
  DomainCell& Emplace(DomainState& state, u64 cell);
  void Erase(DomainState& state, u64 cell);

  void DrainLoader();
  void UpdateDomain(Domain domain, std::span<const scene::WorldStreamObservation> observers);
  void GatherClaims(Domain domain, DomainState& state);
  // Folds this tick's per-observer demands into one candidate per cell: nearest
  // distance wins, the resolved tier joins the region's identity, claims raise
  // the priority, and cells that have failed too often are dropped.
  void MergeCandidates(Domain domain, DomainState& state);
  void NoteLoadFailure(DomainState& state, u64 cell);
  bool Suppressed(const DomainState& state, u64 cell) const;
  void AdvanceRetirements(Domain domain, DomainState& state);

  bool ResolveSchema(DomainCell& cell, std::string* error) const;
  // Materializes at most `rows` rows; true when the cell is fully published.
  bool MaterializeStep(DomainCell& cell, u32 rows);
  // Destroys at most `rows` rows; true when nothing of the cell is left.
  bool TeardownStep(DomainCell& cell, u32 rows);
  void RecordError(std::string message);

  const WorldMap& map_;
  CellLoader& loader_;
  ecs::World& world_;
  const WorldOverlay* overlay_ = nullptr;
  const ClaimSet* claims_ = nullptr;
  WorldStreamPolicy policy_;
  DomainState domains_[kDomainCount];
  base::Vector<CellLoadResult> results_scratch_;
  base::Vector<u32> rows_scratch_;
  base::Vector<CellDemand> claim_scratch_;
  base::Vector<std::string> errors_;
  u32 error_count_ = 0;
  u64 tick_ = 0;
  bool shut_down_ = false;
};

// The reflected layout of a component as the running build sees it, in the same
// terms HashComponentLayout hashes at cook time. A cook and a runtime that
// disagree here disagree about the bytes of the struct.
RX_WORLD_EXPORT bool RuntimeComponentLayout(std::string_view component, u32* stride,
                                            u64* layout_hash);

}  // namespace rx::world

#endif  // RX_WORLD_WORLD_STREAM_H_
