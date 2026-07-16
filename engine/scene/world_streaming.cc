#include "scene/world_streaming.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace rx::scene {
namespace {

constexpr f32 kDistanceQuantization = 1024.0f;

bool IsFinite(const Vec3& value) {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

f32 NonNegative(f32 value) { return std::isfinite(value) && value > 0 ? value : 0; }

u8 SanitizeAxes(u8 axes) {
  axes &= kWorldStreamXYZ;
  return axes == 0 ? static_cast<u8>(kWorldStreamXYZ) : axes;
}

WorldStreamObservation SanitizeObservation(WorldStreamObservation observer) {
  observer.axes = SanitizeAxes(observer.axes);
  auto sanitize_axis = [&](u8 axis, f32* position, f32* velocity) {
    if ((observer.axes & axis) == 0 && !std::isfinite(*position)) *position = 0;
    if (!std::isfinite(*velocity)) *velocity = 0;
  };
  sanitize_axis(kWorldStreamX, &observer.position.x, &observer.velocity.x);
  sanitize_axis(kWorldStreamY, &observer.position.y, &observer.velocity.y);
  sanitize_axis(kWorldStreamZ, &observer.position.z, &observer.velocity.z);
  observer.load_distance = NonNegative(observer.load_distance);
  observer.retain_distance =
      std::max(observer.load_distance, NonNegative(observer.retain_distance));
  observer.prediction_seconds = NonNegative(observer.prediction_seconds);
  observer.maximum_prediction_distance = NonNegative(observer.maximum_prediction_distance);
  return observer;
}

bool HasFiniteActivePosition(const WorldStreamObservation& observer) {
  return ((observer.axes & kWorldStreamX) == 0 || std::isfinite(observer.position.x)) &&
         ((observer.axes & kWorldStreamY) == 0 || std::isfinite(observer.position.y)) &&
         ((observer.axes & kWorldStreamZ) == 0 || std::isfinite(observer.position.z));
}

bool SanitizeRegion(const WorldStreamRegion& source, WorldStreamRegion* result) {
  if (!IsFinite(source.minimum) || !IsFinite(source.maximum)) return false;
  *result = source;
  result->minimum = {std::min(source.minimum.x, source.maximum.x),
                     std::min(source.minimum.y, source.maximum.y),
                     std::min(source.minimum.z, source.maximum.z)};
  result->maximum = {std::max(source.minimum.x, source.maximum.x),
                     std::max(source.minimum.y, source.maximum.y),
                     std::max(source.minimum.z, source.maximum.z)};
  return true;
}

f32 SafeDistance(double distance) {
  if (!std::isfinite(distance) || distance > std::numeric_limits<f32>::max()) {
    return std::numeric_limits<f32>::infinity();
  }
  return static_cast<f32>(std::max(0.0, distance));
}

Vec3 PredictedPosition(const WorldStreamObservation& source) {
  const WorldStreamObservation observer = SanitizeObservation(source);
  if (observer.maximum_prediction_distance <= 0 || observer.prediction_seconds <= 0) {
    return observer.position;
  }

  double offset[3] = {
      (observer.axes & kWorldStreamX)
          ? static_cast<double>(observer.velocity.x) * observer.prediction_seconds
          : 0.0,
      (observer.axes & kWorldStreamY)
          ? static_cast<double>(observer.velocity.y) * observer.prediction_seconds
          : 0.0,
      (observer.axes & kWorldStreamZ)
          ? static_cast<double>(observer.velocity.z) * observer.prediction_seconds
          : 0.0,
  };
  const double length = std::hypot(offset[0], offset[1], offset[2]);
  if (length <= 0) return observer.position;
  const double scale =
      std::min(1.0, static_cast<double>(observer.maximum_prediction_distance) / length);
  const f32 maximum = std::numeric_limits<f32>::max();
  auto advance = [&](f32 position, double delta) {
    const double value = static_cast<double>(position) + delta * scale;
    if (value >= maximum) return maximum;
    if (value <= -maximum) return -maximum;
    return static_cast<f32>(value);
  };
  return {advance(observer.position.x, offset[0]), advance(observer.position.y, offset[1]),
          advance(observer.position.z, offset[2])};
}

f32 PointBoundsDistance(const Vec3& point, const WorldStreamRegion& region, u8 axes) {
  double distance_sq = 0;
  auto accumulate = [&](u8 axis, f32 value, f32 minimum, f32 maximum) {
    if ((axes & axis) == 0) return;
    const double delta = value < minimum   ? static_cast<double>(minimum) - value
                         : value > maximum ? static_cast<double>(value) - maximum
                                           : 0.0;
    distance_sq += delta * delta;
  };
  accumulate(kWorldStreamX, point.x, region.minimum.x, region.maximum.x);
  accumulate(kWorldStreamY, point.y, region.minimum.y, region.maximum.y);
  accumulate(kWorldStreamZ, point.z, region.minimum.z, region.maximum.z);
  return SafeDistance(std::sqrt(distance_sq));
}

double PointSegmentDistance(double point_x, double point_y, double point_z, Vec3 start, Vec3 end,
                            u8 axes) {
  if ((axes & kWorldStreamX) == 0) point_x = start.x = end.x = 0;
  if ((axes & kWorldStreamY) == 0) point_y = start.y = end.y = 0;
  if ((axes & kWorldStreamZ) == 0) point_z = start.z = end.z = 0;
  const double segment[3] = {static_cast<double>(end.x) - start.x,
                             static_cast<double>(end.y) - start.y,
                             static_cast<double>(end.z) - start.z};
  const double relative[3] = {point_x - start.x, point_y - start.y, point_z - start.z};
  const double length_sq =
      segment[0] * segment[0] + segment[1] * segment[1] + segment[2] * segment[2];
  double t = 0;
  if (length_sq > 0) {
    t = std::clamp(
        (relative[0] * segment[0] + relative[1] * segment[1] + relative[2] * segment[2]) /
            length_sq,
        0.0, 1.0);
  }
  return std::hypot(relative[0] - segment[0] * t, relative[1] - segment[1] * t,
                    relative[2] - segment[2] * t);
}

// A bounding sphere around the AABB gives a conservative swept test: it may
// prefetch a corner early, but cannot miss a region crossed between frames.
f32 SweptBoundsDistance(const Vec3& start, const Vec3& end, const WorldStreamRegion& region,
                        u8 axes) {
  const bool moved = ((axes & kWorldStreamX) && start.x != end.x) ||
                     ((axes & kWorldStreamY) && start.y != end.y) ||
                     ((axes & kWorldStreamZ) && start.z != end.z);
  if (!moved) return PointBoundsDistance(start, region, axes);

  const double center[3] = {
      (static_cast<double>(region.minimum.x) + region.maximum.x) * 0.5,
      (static_cast<double>(region.minimum.y) + region.maximum.y) * 0.5,
      (static_cast<double>(region.minimum.z) + region.maximum.z) * 0.5,
  };
  double extent[3] = {
      (axes & kWorldStreamX) ? (static_cast<double>(region.maximum.x) - region.minimum.x) * 0.5
                             : 0.0,
      (axes & kWorldStreamY) ? (static_cast<double>(region.maximum.y) - region.minimum.y) * 0.5
                             : 0.0,
      (axes & kWorldStreamZ) ? (static_cast<double>(region.maximum.z) - region.minimum.z) * 0.5
                             : 0.0,
  };
  const double radius = std::hypot(extent[0], extent[1], extent[2]);
  const double distance = PointSegmentDistance(center[0], center[1], center[2], start, end, axes);
  if (!std::isfinite(distance) || !std::isfinite(radius)) {
    return PointBoundsDistance(start, region, axes);
  }
  return SafeDistance(std::max(0.0, distance - radius));
}

u64 DistanceKey(f32 distance) {
  if (std::isnan(distance) || distance <= 0) return 0;
  // Keep millimetre-scale ordering at ordinary world distances. Above the
  // fixed-point range, positive IEEE-754 bits are monotonic, so the second
  // range preserves ordering through every finite f32 value and infinity.
  constexpr f32 kLinearLimit =
      static_cast<f32>(u64{1} << 32) / kDistanceQuantization;
  if (distance < kLinearLimit) {
    return static_cast<u64>(distance * kDistanceQuantization + 0.5f);
  }
  const u32 bits = std::bit_cast<u32>(distance);
  const u32 limit_bits = std::bit_cast<u32>(kLinearLimit);
  return (u64{1} << 32) + static_cast<u64>(bits - limit_bits);
}

struct ScoredRegion {
  WorldStreamRegion region;
  WorldStreamDemand demand;
  u8 urgency = 2;
  u64 last_prepare_tick = 0;
  u64 last_commit_tick = 0;
};

struct ResidentRetirement {
  u64 id = 0;
  u64 request_tick = 0;
  bool retry_after = false;
  bool retry_within_retain = false;
};

WorldStreamDemand DemandFromObservers(std::span<const WorldStreamObservation> observers,
                                      const WorldStreamRegion& region) {
  WorldStreamDemand combined;
  combined.current_distance = std::numeric_limits<f32>::infinity();
  combined.predicted_distance = std::numeric_limits<f32>::infinity();
  bool any_current_load = false;
  for (const WorldStreamObservation& observer : observers) {
    if ((observer.channels & region.channels) == 0) continue;
    const WorldStreamDemand demand = EvaluateWorldStreamDemand(observer, region);
    combined.load |= demand.load;
    combined.retain |= demand.retain;
    any_current_load |= demand.load && !demand.prediction_only;
    combined.current_distance = std::min(combined.current_distance, demand.current_distance);
    combined.predicted_distance = std::min(combined.predicted_distance, demand.predicted_distance);
  }
  combined.prediction_only = combined.load && !any_current_load;
  return combined;
}

f32 PriorityDistance(const WorldStreamDemand& demand) {
  return std::min(demand.current_distance, demand.predicted_distance);
}

u8 Urgency(const WorldStreamDemand& demand) {
  if (!demand.load) return 2;
  return demand.prediction_only ? 1 : 0;
}

bool RegionMetadataLess(const WorldStreamRegion& a, const WorldStreamRegion& b) {
  if (a.id != b.id) return a.id < b.id;
  if (a.priority != b.priority) return a.priority > b.priority;
  if (a.channels != b.channels) return a.channels < b.channels;
  const f32 av[] = {a.minimum.x, a.minimum.y, a.minimum.z, a.maximum.x, a.maximum.y, a.maximum.z};
  const f32 bv[] = {b.minimum.x, b.minimum.y, b.minimum.z, b.maximum.x, b.maximum.y, b.maximum.z};
  for (u32 i = 0; i < 6; ++i) {
    if (av[i] != bv[i]) return av[i] < bv[i];
  }
  return false;
}

bool RegionPayloadEqual(const WorldStreamRegion& a, const WorldStreamRegion& b) {
  return a.id == b.id && a.channels == b.channels && a.minimum.x == b.minimum.x &&
         a.minimum.y == b.minimum.y && a.minimum.z == b.minimum.z && a.maximum.x == b.maximum.x &&
         a.maximum.y == b.maximum.y && a.maximum.z == b.maximum.z;
}

bool PreparePriorityLess(const ScoredRegion& a, const ScoredRegion& b) {
  if (a.urgency != b.urgency) return a.urgency < b.urgency;
  if (a.region.priority != b.region.priority) return a.region.priority > b.region.priority;
  if (a.last_prepare_tick != b.last_prepare_tick) {
    return a.last_prepare_tick < b.last_prepare_tick;
  }
  const u64 a_distance = DistanceKey(PriorityDistance(a.demand));
  const u64 b_distance = DistanceKey(PriorityDistance(b.demand));
  if (a_distance != b_distance) return a_distance < b_distance;
  return a.region.id < b.region.id;
}

bool CommitPriorityLess(const ScoredRegion& a, const ScoredRegion& b) {
  if (a.urgency != b.urgency) return a.urgency < b.urgency;
  if (a.last_commit_tick != b.last_commit_tick) return a.last_commit_tick < b.last_commit_tick;
  if (a.region.priority != b.region.priority) return a.region.priority > b.region.priority;
  const u64 a_distance = DistanceKey(PriorityDistance(a.demand));
  const u64 b_distance = DistanceKey(PriorityDistance(b.demand));
  if (a_distance != b_distance) return a_distance < b_distance;
  return a.region.id < b.region.id;
}

WorldStreamAction ActionFor(WorldStreamActionKind kind, const WorldStreamRegion& region,
                            u64 generation, const WorldStreamDemand& demand = {}) {
  return {kind, {region.id, generation}, region, PriorityDistance(demand), demand.prediction_only};
}

}  // namespace

WorldStreamPlan::TrackedRegion* WorldStreamPlan::FindRegion(u64 id) {
  if (regions_.empty()) return nullptr;
  auto it = std::lower_bound(
      regions_.begin(), regions_.end(), id,
      [](const TrackedRegion& region, u64 wanted) { return region.region.id < wanted; });
  return it != regions_.end() && it->region.id == id ? it : nullptr;
}

const WorldStreamPlan::TrackedRegion* WorldStreamPlan::FindRegion(u64 id) const {
  if (regions_.empty()) return nullptr;
  auto it = std::lower_bound(
      regions_.begin(), regions_.end(), id,
      [](const TrackedRegion& region, u64 wanted) { return region.region.id < wanted; });
  return it != regions_.end() && it->region.id == id ? it : nullptr;
}

WorldStreamPlan::TrackedRegion* WorldStreamPlan::FindTicket(WorldStreamTicket ticket) {
  TrackedRegion* tracked = FindRegion(ticket.region);
  return tracked && tracked->generation == ticket.generation ? tracked : nullptr;
}

const WorldStreamPlan::TrackedRegion* WorldStreamPlan::FindTicket(WorldStreamTicket ticket) const {
  const TrackedRegion* tracked = FindRegion(ticket.region);
  return tracked && tracked->generation == ticket.generation ? tracked : nullptr;
}

void WorldStreamPlan::EraseRegion(u64 id) {
  TrackedRegion* tracked = FindRegion(id);
  if (tracked) regions_.erase(tracked);
}

u64 WorldStreamPlan::TakeGeneration() {
  const u64 generation = next_generation_++;
  if (next_generation_ == 0) next_generation_ = 1;
  return generation == 0 ? next_generation_++ : generation;
}

WorldStreamObservation MakeWorldStreamObservation(const Transform& transform,
                                                  const WorldStreamObserver& observer) {
  return SanitizeObservation({{transform.position[0], transform.position[1], transform.position[2]},
                              observer.velocity,
                              observer.load_distance,
                              observer.retain_distance,
                              observer.prediction_seconds,
                              observer.maximum_prediction_distance,
                              observer.channels,
                              observer.axes});
}

WorldStreamQuery BuildWorldStreamQuery(const WorldStreamObservation& source) {
  const WorldStreamObservation observer = SanitizeObservation(source);
  if (!HasFiniteActivePosition(observer)) {
    return {{}, {}, 0, 0, observer.axes};
  }
  return {observer.position, PredictedPosition(observer), observer.retain_distance,
          observer.channels, observer.axes};
}

WorldStreamDemand EvaluateWorldStreamDemand(const WorldStreamObservation& source,
                                            const WorldStreamRegion& source_region) {
  const WorldStreamObservation observer = SanitizeObservation(source);
  WorldStreamRegion region;
  if (!HasFiniteActivePosition(observer) || !SanitizeRegion(source_region, &region) ||
      (observer.channels & source_region.channels) == 0) {
    return {false, false, false, std::numeric_limits<f32>::infinity(),
            std::numeric_limits<f32>::infinity()};
  }

  const Vec3 predicted = PredictedPosition(observer);
  const f32 current = PointBoundsDistance(observer.position, region, observer.axes);
  const f32 swept = SweptBoundsDistance(observer.position, predicted, region, observer.axes);
  const f32 prediction = std::min(current, swept);
  const bool load = prediction <= observer.load_distance;
  return {load, prediction <= observer.retain_distance, load && current > observer.load_distance,
          current, prediction};
}

void GatherWorldStreamObservers(ecs::World& world,
                                base::Vector<WorldStreamObservation>* observations) {
  if (!observations) return;
  observations->clear();
  world.Each<WorldStreamObserver, Transform>(
      [&](ecs::Entity entity, WorldStreamObserver& observer, Transform& transform) {
        if (world.Has<Parent>(entity)) return;
        WorldStreamObservation observation = MakeWorldStreamObservation(transform, observer);
        if (HasFiniteActivePosition(observation)) observations->push_back(observation);
      });
}

void ConfigureWorldStreaming(WorldStreamPlan& plan, const WorldStreamSettings& settings) {
  plan.settings_ = settings;
}

void AdvanceWorldStreaming(WorldStreamPlan& plan, std::span<const WorldStreamObservation> observers,
                           std::span<const WorldStreamRegion> candidates,
                           const WorldStreamFrameBudget& budget,
                           base::Vector<WorldStreamAction>* actions) {
  if (!actions) return;
  actions->clear();
  ++plan.tick_;

  if (plan.resetting_) {
    if (plan.regions_.empty()) {
      plan.resetting_ = false;
    } else {
      for (WorldStreamPlan::TrackedRegion& tracked : plan.regions_) {
        if (!tracked.retire_action_pending) continue;
        actions->push_back(ActionFor(tracked.retire_action, tracked.region, tracked.generation));
        tracked.retire_action_pending = false;
      }
      return;
    }
  }

  base::Vector<WorldStreamRegion> canonical;
  canonical.reserve(candidates.size());
  for (const WorldStreamRegion& source : candidates) {
    WorldStreamRegion region;
    if (SanitizeRegion(source, &region)) canonical.push_back(region);
  }
  std::sort(canonical.begin(), canonical.end(), RegionMetadataLess);
  canonical.erase(std::unique(canonical.begin(), canonical.end(),
                              [](const WorldStreamRegion& a, const WorldStreamRegion& b) {
                                return a.id == b.id;
                              }),
                  canonical.end());

  base::Vector<ScoredRegion> scored;
  scored.reserve(canonical.size());
  for (const WorldStreamRegion& region : canonical) {
    ScoredRegion score;
    score.region = region;
    score.demand = DemandFromObservers(observers, region);
    score.urgency = Urgency(score.demand);
    if (const WorldStreamPlan::TrackedRegion* tracked = plan.FindRegion(region.id)) {
      score.last_prepare_tick = tracked->last_prepare_tick;
      score.last_commit_tick = tracked->last_commit_tick;
    }
    scored.push_back(score);
  }

  auto score_for = [&](u64 id) -> ScoredRegion* {
    if (scored.empty()) return nullptr;
    auto it = std::lower_bound(
        scored.begin(), scored.end(), id,
        [](const ScoredRegion& score, u64 wanted) { return score.region.id < wanted; });
    return it != scored.end() && it->region.id == id ? it : nullptr;
  };

  // Forget wait age when demand disappears or a lifecycle generation now owns
  // the region. The remaining entries stay sorted by id.
  for (size_t i = 0; i < plan.waiting_.size();) {
    const ScoredRegion* score = score_for(plan.waiting_[i].id);
    if (!score || !score->demand.load || plan.FindRegion(plan.waiting_[i].id)) {
      plan.waiting_.erase(i);
    } else {
      ++i;
    }
  }

  base::Vector<u64> erase_ids;
  base::Vector<ScoredRegion> prepare;
  base::Vector<ScoredRegion> commit;
  base::Vector<ResidentRetirement> resident_retirements;
  u32 pending = 0;

  for (WorldStreamPlan::TrackedRegion& tracked : plan.regions_) {
    ScoredRegion* current = score_for(tracked.region.id);
    WorldStreamDemand demand;
    demand.current_distance = std::numeric_limits<f32>::infinity();
    demand.predicted_distance = std::numeric_limits<f32>::infinity();
    if (current) demand = current->demand;
    if (tracked.phase == WorldStreamPlan::Phase::kPreparing ||
        tracked.phase == WorldStreamPlan::Phase::kReady ||
        tracked.phase == WorldStreamPlan::Phase::kCommitting ||
        tracked.phase == WorldStreamPlan::Phase::kRetiring) {
      ++pending;
    }

    if (tracked.phase == WorldStreamPlan::Phase::kRetiring) {
      tracked.retry_after_retire = tracked.retry_within_retain ? demand.retain : demand.load;
      continue;
    }

    if (current && !RegionPayloadEqual(tracked.region, current->region)) {
      if (tracked.phase == WorldStreamPlan::Phase::kFailed) {
        tracked.region = current->region;
        tracked.retry_at_tick = plan.tick_;
      } else {
        const bool resident = tracked.phase == WorldStreamPlan::Phase::kResident;
        if (resident) {
          if (tracked.retire_request_tick == 0) tracked.retire_request_tick = plan.tick_;
          resident_retirements.push_back(
              {tracked.region.id, tracked.retire_request_tick, demand.retain, true});
        } else {
          tracked.phase = WorldStreamPlan::Phase::kRetiring;
          tracked.retire_action = WorldStreamActionKind::kCancel;
          tracked.retire_action_pending = true;
          tracked.retry_after_retire = demand.retain;
          tracked.retry_within_retain = true;
          tracked.retry_at_tick = plan.tick_;
        }
        continue;
      }
    }

    if (current) tracked.region.priority = current->region.priority;

    if (!demand.retain) {
      if (tracked.phase == WorldStreamPlan::Phase::kFailed) {
        erase_ids.push_back(tracked.region.id);
      } else {
        const bool resident = tracked.phase == WorldStreamPlan::Phase::kResident;
        if (resident) {
          if (tracked.retire_request_tick == 0) tracked.retire_request_tick = plan.tick_;
          resident_retirements.push_back(
              {tracked.region.id, tracked.retire_request_tick, false, false});
        } else {
          tracked.phase = WorldStreamPlan::Phase::kRetiring;
          tracked.retire_action = WorldStreamActionKind::kCancel;
          tracked.retire_action_pending = true;
          tracked.retry_after_retire = false;
          tracked.retry_within_retain = false;
          tracked.retry_at_tick = plan.tick_;
        }
      }
      continue;
    }

    tracked.retire_request_tick = 0;

    if (tracked.phase == WorldStreamPlan::Phase::kFailed && demand.retain &&
        plan.tick_ >= tracked.retry_at_tick) {
      if (current) tracked.region = current->region;
      prepare.push_back({tracked.region, demand, Urgency(demand), tracked.last_prepare_tick,
                         tracked.last_commit_tick});
    } else if (tracked.phase == WorldStreamPlan::Phase::kReady) {
      commit.push_back({tracked.region, demand, Urgency(demand), tracked.last_prepare_tick,
                        tracked.last_commit_tick});
    }
  }
  for (u64 id : erase_ids) plan.EraseRegion(id);

  // Cancellation is unbudgeted: invalidating stale work promptly is cheaper
  // than allowing unwanted I/O, decompression, or integration to continue.
  for (WorldStreamPlan::TrackedRegion& tracked : plan.regions_) {
    if (tracked.phase != WorldStreamPlan::Phase::kRetiring ||
        tracked.retire_action != WorldStreamActionKind::kCancel || !tracked.retire_action_pending) {
      continue;
    }
    actions->push_back(
        ActionFor(WorldStreamActionKind::kCancel, tracked.region, tracked.generation));
    tracked.retire_action_pending = false;
  }

  for (ScoredRegion score : scored) {
    if (!score.demand.load || plan.FindRegion(score.region.id)) continue;
    auto waiting = std::lower_bound(
        plan.waiting_.begin(), plan.waiting_.end(), score.region.id,
        [](const WorldStreamPlan::WaitingRegion& region, u64 wanted) { return region.id < wanted; });
    if (waiting == plan.waiting_.end() || waiting->id != score.region.id) {
      waiting = plan.waiting_.insert(waiting, {score.region.id, plan.tick_});
    }
    score.last_prepare_tick = waiting->since_tick;
    prepare.push_back(score);
  }

  // Admit cleanup before new work so continual demand cannot keep obsolete
  // resident payloads alive by consuming every pending slot first.
  std::sort(resident_retirements.begin(), resident_retirements.end(),
            [](const ResidentRetirement& a, const ResidentRetirement& b) {
              if (a.request_tick != b.request_tick) return a.request_tick < b.request_tick;
              return a.id < b.id;
            });
  u32 unloads = 0;
  for (const ResidentRetirement& retirement : resident_retirements) {
    if (unloads >= budget.maximum_unloads || pending >= budget.maximum_pending) break;
    WorldStreamPlan::TrackedRegion* tracked = plan.FindRegion(retirement.id);
    if (!tracked || tracked->phase != WorldStreamPlan::Phase::kResident) continue;
    tracked->phase = WorldStreamPlan::Phase::kRetiring;
    tracked->retire_action = WorldStreamActionKind::kUnload;
    tracked->retire_action_pending = false;
    tracked->retry_after_retire = retirement.retry_after;
    tracked->retry_within_retain = retirement.retry_within_retain;
    tracked->retry_at_tick = plan.tick_;
    actions->push_back(
        ActionFor(WorldStreamActionKind::kUnload, tracked->region, tracked->generation));
    ++unloads;
    ++pending;
  }

  std::sort(prepare.begin(), prepare.end(), PreparePriorityLess);
  u32 prepare_starts = 0;
  for (const ScoredRegion& score : prepare) {
    if (prepare_starts >= budget.maximum_prepare_starts || pending >= budget.maximum_pending) {
      break;
    }
    WorldStreamPlan::TrackedRegion* tracked = plan.FindRegion(score.region.id);
    if (!tracked) {
      WorldStreamPlan::TrackedRegion created;
      created.region = score.region;
      created.generation = plan.TakeGeneration();
      auto insertion = std::lower_bound(plan.regions_.begin(), plan.regions_.end(), score.region.id,
                                        [](const WorldStreamPlan::TrackedRegion& region,
                                           u64 wanted) { return region.region.id < wanted; });
      tracked = plan.regions_.insert(insertion, created);
      auto waiting = std::lower_bound(
          plan.waiting_.begin(), plan.waiting_.end(), score.region.id,
          [](const WorldStreamPlan::WaitingRegion& region, u64 wanted) {
            return region.id < wanted;
          });
      if (waiting != plan.waiting_.end() && waiting->id == score.region.id) {
        plan.waiting_.erase(waiting);
      }
    } else {
      tracked->region = score.region;
      tracked->generation = plan.TakeGeneration();
      tracked->phase = WorldStreamPlan::Phase::kPreparing;
      tracked->retire_action_pending = false;
      tracked->retry_after_retire = false;
      tracked->retry_within_retain = false;
    }
    actions->push_back(ActionFor(WorldStreamActionKind::kPrepare, tracked->region,
                                 tracked->generation, score.demand));
    tracked->last_prepare_tick = plan.tick_;
    ++prepare_starts;
    ++pending;
  }

  std::sort(commit.begin(), commit.end(), CommitPriorityLess);
  u32 commit_steps = 0;
  for (const ScoredRegion& score : commit) {
    if (commit_steps >= budget.maximum_commit_steps) break;
    WorldStreamPlan::TrackedRegion* tracked = plan.FindRegion(score.region.id);
    if (!tracked || tracked->phase != WorldStreamPlan::Phase::kReady) continue;
    tracked->phase = WorldStreamPlan::Phase::kCommitting;
    tracked->last_commit_tick = plan.tick_;
    actions->push_back(ActionFor(WorldStreamActionKind::kCommit, tracked->region,
                                 tracked->generation, score.demand));
    ++commit_steps;
  }
}

bool ApplyWorldStreamPrepareResult(WorldStreamPlan& plan, WorldStreamTicket ticket,
                                   WorldStreamPrepareResult result) {
  WorldStreamPlan::TrackedRegion* tracked = plan.FindTicket(ticket);
  if (!tracked || tracked->phase != WorldStreamPlan::Phase::kPreparing) return false;
  if (result == WorldStreamPrepareResult::kReady) {
    tracked->phase = WorldStreamPlan::Phase::kReady;
    tracked->last_commit_tick = plan.tick_;
  } else {
    // Preparation may have produced partial payload or in-flight work. Require
    // the shell to clean that generation before admitting a retry.
    tracked->phase = WorldStreamPlan::Phase::kRetiring;
    tracked->retire_action = WorldStreamActionKind::kCancel;
    tracked->retire_action_pending = true;
    tracked->retry_after_retire = true;
    tracked->retry_within_retain = true;
    tracked->retry_at_tick = plan.tick_ + plan.settings_.retry_delay_ticks;
  }
  return true;
}

bool ApplyWorldStreamCommitResult(WorldStreamPlan& plan, WorldStreamTicket ticket,
                                  WorldStreamCommitResult result) {
  WorldStreamPlan::TrackedRegion* tracked = plan.FindTicket(ticket);
  if (!tracked || tracked->phase != WorldStreamPlan::Phase::kCommitting) return false;
  if (result == WorldStreamCommitResult::kComplete) {
    tracked->phase = WorldStreamPlan::Phase::kResident;
  } else if (result == WorldStreamCommitResult::kMoreWork) {
    tracked->phase = WorldStreamPlan::Phase::kReady;
  } else {
    tracked->phase = WorldStreamPlan::Phase::kRetiring;
    tracked->retire_action = WorldStreamActionKind::kCancel;
    tracked->retire_action_pending = true;
    tracked->retry_after_retire = true;
    tracked->retry_within_retain = true;
    tracked->retry_at_tick = plan.tick_ + plan.settings_.retry_delay_ticks;
  }
  return true;
}

bool ApplyWorldStreamRetireResult(WorldStreamPlan& plan, WorldStreamTicket ticket) {
  WorldStreamPlan::TrackedRegion* tracked = plan.FindTicket(ticket);
  if (!tracked || tracked->phase != WorldStreamPlan::Phase::kRetiring ||
      tracked->retire_action_pending) {
    return false;
  }
  if (tracked->retry_after_retire && !plan.resetting_) {
    tracked->phase = WorldStreamPlan::Phase::kFailed;
    tracked->generation = 0;
    tracked->retire_action_pending = false;
    tracked->retry_after_retire = false;
    tracked->retry_within_retain = false;
  } else {
    plan.EraseRegion(ticket.region);
  }
  return true;
}

bool IsWorldStreamTicketCurrent(const WorldStreamPlan& plan, WorldStreamTicket ticket) {
  const WorldStreamPlan::TrackedRegion* tracked = plan.FindTicket(ticket);
  return tracked && tracked->phase != WorldStreamPlan::Phase::kRetiring &&
         tracked->phase != WorldStreamPlan::Phase::kFailed;
}

WorldStreamStats GetWorldStreamStats(const WorldStreamPlan& plan) {
  WorldStreamStats stats;
  stats.tick = plan.tick_;
  for (const WorldStreamPlan::TrackedRegion& tracked : plan.regions_) {
    switch (tracked.phase) {
      case WorldStreamPlan::Phase::kPreparing:
        ++stats.preparing;
        break;
      case WorldStreamPlan::Phase::kReady:
        ++stats.ready;
        break;
      case WorldStreamPlan::Phase::kCommitting:
        ++stats.committing;
        break;
      case WorldStreamPlan::Phase::kResident:
        ++stats.resident;
        break;
      case WorldStreamPlan::Phase::kRetiring:
        ++stats.retiring;
        break;
      case WorldStreamPlan::Phase::kFailed:
        ++stats.failed;
        break;
    }
  }
  return stats;
}

void ResetWorldStreaming(WorldStreamPlan& plan, base::Vector<WorldStreamAction>* actions) {
  if (!actions) return;
  actions->clear();
  plan.resetting_ = true;
  plan.waiting_.clear();
  base::Vector<u64> erase_ids;
  for (WorldStreamPlan::TrackedRegion& tracked : plan.regions_) {
    tracked.retry_after_retire = false;
    tracked.retry_within_retain = false;
    if (tracked.phase == WorldStreamPlan::Phase::kFailed) {
      erase_ids.push_back(tracked.region.id);
      continue;
    }
    if (tracked.phase != WorldStreamPlan::Phase::kRetiring) {
      tracked.retire_action = tracked.phase == WorldStreamPlan::Phase::kResident
                                  ? WorldStreamActionKind::kUnload
                                  : WorldStreamActionKind::kCancel;
      tracked.phase = WorldStreamPlan::Phase::kRetiring;
      tracked.retire_action_pending = true;
    }
    if (tracked.retire_action_pending) {
      actions->push_back(ActionFor(tracked.retire_action, tracked.region, tracked.generation));
      tracked.retire_action_pending = false;
    }
  }
  for (u64 id : erase_ids) plan.EraseRegion(id);
  if (plan.regions_.empty()) plan.resetting_ = false;
}

}  // namespace rx::scene
