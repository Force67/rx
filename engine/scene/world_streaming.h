#ifndef RX_SCENE_WORLD_STREAMING_H_
#define RX_SCENE_WORLD_STREAMING_H_

#include <base/containers/vector.h>

#include <span>

#include "core/export.h"
#include "core/math.h"
#include "ecs/world.h"
#include "scene/components.h"

namespace rx::scene {

enum WorldStreamAxis : u8 {
  kWorldStreamX = 1 << 0,
  kWorldStreamY = 1 << 1,
  kWorldStreamZ = 1 << 2,
  kWorldStreamXZ = kWorldStreamX | kWorldStreamZ,
  kWorldStreamXYZ = kWorldStreamX | kWorldStreamY | kWorldStreamZ,
};

// Attach to a parent-free Transform to make an entity a streaming source.
// Content enters at load_distance and remains requested or resident until it
// leaves the larger retain_distance. Prediction adds a conservative swept
// volume ahead of velocity; it never removes the omnidirectional baseline.
struct WorldStreamObserver {
  Vec3 velocity;
  f32 load_distance = 0;
  f32 retain_distance = 0;
  f32 prediction_seconds = 0;
  f32 maximum_prediction_distance = 0;
  u32 channels = ~u32{0};
  u8 axes = kWorldStreamXYZ;
};

struct WorldStreamObservation {
  Vec3 position;
  Vec3 velocity;
  f32 load_distance = 0;
  f32 retain_distance = 0;
  f32 prediction_seconds = 0;
  f32 maximum_prediction_distance = 0;
  u32 channels = ~u32{0};
  u8 axes = kWorldStreamXYZ;
};

// Conservative broad-phase query. A spatial index should return regions
// overlapping this swept sphere; EvaluateWorldStreamDemand performs the final
// region-bounds test. One query is produced per observer and channel mask.
struct WorldStreamQuery {
  Vec3 origin;
  Vec3 predicted;
  f32 radius = 0;
  u32 channels = ~u32{0};
  u8 axes = kWorldStreamXYZ;
};

// Content-neutral spatial unit. It may describe a grid cell, terrain tile,
// dungeon room, procedural sector, or another stable package of world state.
struct WorldStreamRegion {
  u64 id = 0;
  Vec3 minimum;
  Vec3 maximum;
  i32 priority = 0;
  u32 channels = ~u32{0};
};

struct WorldStreamDemand {
  bool load = false;
  bool retain = false;
  bool prediction_only = false;
  f32 current_distance = 0;
  f32 predicted_distance = 0;
};

struct WorldStreamTicket {
  u64 region = 0;
  u64 generation = 0;

  bool operator==(const WorldStreamTicket&) const = default;
};

enum class WorldStreamActionKind : u8 {
  kPrepare,
  kCancel,
  kCommit,
  kUnload,
};

struct WorldStreamAction {
  WorldStreamActionKind kind = WorldStreamActionKind::kPrepare;
  WorldStreamTicket ticket;
  WorldStreamRegion region;
  f32 distance = 0;
  bool prediction_only = false;
};

// Each commit action grants one consumer-defined, bounded quantum. Consumers
// must not interpret it as permission to instantiate an unbounded whole region.
struct WorldStreamFrameBudget {
  u32 maximum_prepare_starts = 2;
  u32 maximum_commit_steps = 2;
  u32 maximum_unloads = 8;
  // Preparing, prepared, committing, and retiring regions all count toward
  // this cap because each may retain CPU payload or asynchronous work.
  u32 maximum_pending = 8;
};

struct WorldStreamSettings {
  u32 retry_delay_ticks = 30;
};

enum class WorldStreamPrepareResult : u8 {
  kReady,
  kFailed,
};

enum class WorldStreamCommitResult : u8 {
  kComplete,
  kMoreWork,
  kFailed,
};

struct WorldStreamStats {
  u64 tick = 0;
  u32 preparing = 0;
  u32 ready = 0;
  u32 committing = 0;
  u32 resident = 0;
  u32 retiring = 0;
  u32 failed = 0;
};

// Explicit state for the functional planning core. The state is deliberately
// separate from jobs, assets, rendering, physics, and ECS mutation. Before
// destroying a live plan, the shell must ResetWorldStreaming and acknowledge
// every emitted retirement so its own jobs and payloads are not abandoned.
class WorldStreamPlan {
 public:
  WorldStreamPlan() = default;
  WorldStreamPlan(const WorldStreamPlan&) = delete;
  WorldStreamPlan& operator=(const WorldStreamPlan&) = delete;

 private:
  enum class Phase : u8 {
    kPreparing,
    kReady,
    kCommitting,
    kResident,
    kRetiring,
    kFailed,
  };

  struct TrackedRegion {
    WorldStreamRegion region;
    u64 generation = 0;
    u64 retry_at_tick = 0;
    u64 last_prepare_tick = 0;
    u64 last_commit_tick = 0;
    u64 retire_request_tick = 0;
    Phase phase = Phase::kPreparing;
    WorldStreamActionKind retire_action = WorldStreamActionKind::kCancel;
    bool retire_action_pending = false;
    bool retry_after_retire = false;
    bool retry_within_retain = false;
  };

  struct WaitingRegion {
    u64 id = 0;
    u64 since_tick = 0;
  };

  WorldStreamSettings settings_;
  // Sorted by region id. Observer bubbles contain tens or hundreds of regions;
  // compact iteration is preferable to a churn-sensitive hash table.
  base::Vector<TrackedRegion> regions_;
  // Demand that has not yet acquired a pending slot. Keeping first-seen age
  // prevents continual arrivals from starving an older request.
  base::Vector<WaitingRegion> waiting_;
  u64 tick_ = 0;
  u64 next_generation_ = 1;
  bool resetting_ = false;

  TrackedRegion* FindRegion(u64 id);
  const TrackedRegion* FindRegion(u64 id) const;
  TrackedRegion* FindTicket(WorldStreamTicket ticket);
  const TrackedRegion* FindTicket(WorldStreamTicket ticket) const;
  void EraseRegion(u64 id);
  u64 TakeGeneration();

  friend void ConfigureWorldStreaming(WorldStreamPlan&, const WorldStreamSettings&);
  friend void AdvanceWorldStreaming(WorldStreamPlan&, std::span<const WorldStreamObservation>,
                                    std::span<const WorldStreamRegion>,
                                    const WorldStreamFrameBudget&,
                                    base::Vector<WorldStreamAction>*);
  friend bool ApplyWorldStreamPrepareResult(WorldStreamPlan&, WorldStreamTicket,
                                            WorldStreamPrepareResult);
  friend bool ApplyWorldStreamCommitResult(WorldStreamPlan&, WorldStreamTicket,
                                           WorldStreamCommitResult);
  friend bool ApplyWorldStreamRetireResult(WorldStreamPlan&, WorldStreamTicket);
  friend bool IsWorldStreamTicketCurrent(const WorldStreamPlan&, WorldStreamTicket);
  friend WorldStreamStats GetWorldStreamStats(const WorldStreamPlan&);
  friend void ResetWorldStreaming(WorldStreamPlan&, base::Vector<WorldStreamAction>*);
};

RX_SCENE_EXPORT WorldStreamObservation
MakeWorldStreamObservation(const Transform& transform, const WorldStreamObserver& observer);
RX_SCENE_EXPORT WorldStreamQuery BuildWorldStreamQuery(const WorldStreamObservation& observer);
RX_SCENE_EXPORT WorldStreamDemand EvaluateWorldStreamDemand(const WorldStreamObservation& observer,
                                                            const WorldStreamRegion& region);

// Gathers observer values without structurally mutating the ECS world.
RX_SCENE_EXPORT void GatherWorldStreamObservers(ecs::World& world,
                                                base::Vector<WorldStreamObservation>* observations);

RX_SCENE_EXPORT void ConfigureWorldStreaming(WorldStreamPlan& plan,
                                             const WorldStreamSettings& settings);

// Deterministically advances lifecycle state and replaces actions with this
// tick's side-effect requests. Candidates must be the complete union of the
// observers' retain-radius queries; omission means a region left the catalog.
// Candidate order does not affect action order.
RX_SCENE_EXPORT void AdvanceWorldStreaming(WorldStreamPlan& plan,
                                           std::span<const WorldStreamObservation> observers,
                                           std::span<const WorldStreamRegion> candidates,
                                           const WorldStreamFrameBudget& budget,
                                           base::Vector<WorldStreamAction>* actions);

// Completion ingress is generation checked and owner-thread only. Worker jobs
// publish ticketed results to a queue; the owner applies them before Advance.
// False means the result is stale and its payload must not be committed.
RX_SCENE_EXPORT bool ApplyWorldStreamPrepareResult(WorldStreamPlan& plan, WorldStreamTicket ticket,
                                                   WorldStreamPrepareResult result);
RX_SCENE_EXPORT bool ApplyWorldStreamCommitResult(WorldStreamPlan& plan, WorldStreamTicket ticket,
                                                  WorldStreamCommitResult result);
// A cancel or unload action retires a generation but keeps it tracked until the
// shell confirms that jobs and payload cleanup are complete. Synchronous shells
// may call this immediately after executing the action.
RX_SCENE_EXPORT bool ApplyWorldStreamRetireResult(WorldStreamPlan& plan, WorldStreamTicket ticket);
RX_SCENE_EXPORT bool IsWorldStreamTicketCurrent(const WorldStreamPlan& plan,
                                                WorldStreamTicket ticket);
RX_SCENE_EXPORT WorldStreamStats GetWorldStreamStats(const WorldStreamPlan& plan);

// Begins retiring every generation and emits deterministic cleanup actions. The
// plan becomes empty as the shell acknowledges them with ApplyRetireResult.
RX_SCENE_EXPORT void ResetWorldStreaming(WorldStreamPlan& plan,
                                         base::Vector<WorldStreamAction>* actions);

}  // namespace rx::scene

#endif  // RX_SCENE_WORLD_STREAMING_H_
