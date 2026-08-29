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
#include "world/world_map.h"

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
  u32 prototype = 0;  // index into the owning cell payload's prototype table
  Vec3 position;
  Quat rotation;
  f32 scale = 1.0f;
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
class RX_WORLD_EXPORT CellLoader {
 public:
  virtual ~CellLoader() = default;

  virtual void Begin(const CellLoadRequest& request) = 0;
  virtual void Cancel(scene::WorldStreamTicket ticket) = 0;
  // Moves out everything that finished since the last call.
  virtual void Poll(base::Vector<CellLoadResult>* out) = 0;
};

// Reads and decodes through the Vfs. `map` and `vfs` must outlive the loader.
RX_WORLD_EXPORT base::UniquePointer<CellLoader> MakeArchiveCellLoader(const WorldMap& map,
                                                                      const asset::Vfs& vfs);

struct WorldStreamerStats {
  u32 resident = 0;   // (cell, domain) pairs fully published
  u32 pending = 0;    // reading, decoded-not-published, or retiring
  u32 failed = 0;     // refused payloads awaiting a retry
  u32 entities = 0;   // ECS rows this streamer owns
  u32 instances = 0;  // static instances resident
  u64 resident_bytes = 0;
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

  // One tick. `observers` are world-space streaming sources - the player, the
  // camera, a teleport destination, an AI route request; the streamer derives
  // one bubble per domain from each.
  void Update(std::span<const scene::WorldStreamObservation> observers);

  // Retires everything and drains it in this call, so the ecs::World can be
  // destroyed afterwards. Idempotent.
  void Shutdown();

  // The live entity for a stable id, or a null entity when its cell is not
  // resident. Resolution goes through the index's stable-id ranges, so an id
  // belonging to an unloaded cell is answered without any of it resident.
  ecs::Entity Resolve(u64 stable_id) const;

  // Resident static instances of one cell, empty when it is not resident.
  std::span<const ResidentInstance> Instances(u64 cell) const;
  // The instance carrying `stable_id`, or null.
  const ResidentInstance* FindInstance(u64 stable_id) const;

  // Turns a static instance into a real entity: the point where a rock stops
  // being a row in a page and starts being something with behavior. The
  // instance stays in the page (the renderer keeps drawing it) unless the
  // caller removes it; what it gains is an ECS identity under the same stable
  // id. Returns a null entity when the instance is not resident, or when it
  // was already promoted.
  ecs::Entity Promote(u64 stable_id);

  WorldStreamerStats stats() const;

  // Load failures since construction, oldest first and capped. A refused
  // payload is a cook or archive bug, so the messages are kept rather than
  // counted: knowing which cell and why is the whole value.
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
  };

  enum class CellPhase : u8 {
    kLoading,    // asked the loader, nothing decoded yet
    kDecoded,    // payload validated, waiting for commit quanta
    kPublished,  // fully materialized
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
    base::Vector<u64> promoted;  // stable ids that already have an entity
    u64 resident_bytes = 0;
  };

  struct DomainState {
    scene::WorldStreamPlan plan;
    base::Vector<DomainCell> cells;  // sorted by cell id
    base::Vector<scene::WorldStreamObservation> observations;
    base::Vector<scene::WorldStreamRegion> candidates;
    base::Vector<scene::WorldStreamAction> actions;
  };

  DomainCell* Find(DomainState& state, u64 cell);
  const DomainCell* Find(const DomainState& state, u64 cell) const;
  DomainCell& Emplace(DomainState& state, u64 cell);
  void Erase(DomainState& state, u64 cell);

  void DrainLoader();
  void UpdateDomain(Domain domain, std::span<const scene::WorldStreamObservation> observers);
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
  WorldStreamPolicy policy_;
  DomainState domains_[kDomainCount];
  base::Vector<CellLoadResult> results_scratch_;
  base::Vector<std::string> errors_;
  u32 error_count_ = 0;
  bool shut_down_ = false;
};

// The reflected layout of a component as the running build sees it, in the same
// terms HashComponentLayout hashes at cook time. A cook and a runtime that
// disagree here disagree about the bytes of the struct.
RX_WORLD_EXPORT bool RuntimeComponentLayout(std::string_view component, u32* stride,
                                            u64* layout_hash);

}  // namespace rx::world

#endif  // RX_WORLD_WORLD_STREAM_H_
