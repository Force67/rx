#include "scene/world_streaming.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

using namespace rx::scene;
using rx::f32;
using rx::i32;
using rx::u32;
using rx::u64;
using rx::Vec3;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "world_streaming_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-4f) {
  if (std::abs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "world_streaming_test: FAIL: %s (got %.6f, expected %.6f)\n", message,
               actual, expected);
  ++failures;
}

WorldStreamObservation Observer(Vec3 position, f32 load = 10, f32 retain = 15) {
  WorldStreamObservation observer;
  observer.position = position;
  observer.load_distance = load;
  observer.retain_distance = retain;
  observer.axes = kWorldStreamXZ;
  return observer;
}

WorldStreamRegion Region(u64 id, f32 min_x, f32 min_z, f32 max_x, f32 max_z, i32 priority = 0,
                         u32 channels = ~u32{0}) {
  return {id, {min_x, -100, min_z}, {max_x, 100, max_z}, priority, channels};
}

const WorldStreamAction* FindAction(const base::Vector<WorldStreamAction>& actions,
                                    WorldStreamActionKind kind, u64 id) {
  for (const WorldStreamAction& action : actions) {
    if (action.kind == kind && action.ticket.region == id) return &action;
  }
  return nullptr;
}

WorldStreamTicket PrepareOne(WorldStreamPlan& plan, const WorldStreamObservation& observer,
                             const WorldStreamRegion& region) {
  base::Vector<WorldStreamAction> actions;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&region, 1), {}, &actions);
  Check(actions.size() == 1 && actions[0].kind == WorldStreamActionKind::kPrepare,
        "an entering region emits one prepare");
  return actions.empty() ? WorldStreamTicket{} : actions[0].ticket;
}

void MakeResident(WorldStreamPlan& plan, const WorldStreamObservation& observer,
                  const WorldStreamRegion& region, WorldStreamTicket ticket) {
  Check(ApplyWorldStreamPrepareResult(plan, ticket, WorldStreamPrepareResult::kReady),
        "current prepare result is accepted");
  base::Vector<WorldStreamAction> actions;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&region, 1), {}, &actions);
  const WorldStreamAction* commit = FindAction(actions, WorldStreamActionKind::kCommit, region.id);
  Check(commit != nullptr, "a ready region emits a commit quantum");
  if (commit) {
    Check(ApplyWorldStreamCommitResult(plan, commit->ticket, WorldStreamCommitResult::kComplete),
          "current commit result is accepted");
  }
}

void TestDemandAndQueries() {
  const WorldStreamRegion region = Region(1, 10, -1, 12, 1);
  WorldStreamObservation observer = Observer({0, 0, 0});

  WorldStreamDemand demand = EvaluateWorldStreamDemand(observer, region);
  Check(demand.load && demand.retain, "region bounds enter at the load distance");
  Near(demand.current_distance, 10, "distance is measured to bounds, not the center");

  observer.position.x = -3;
  demand = EvaluateWorldStreamDemand(observer, region);
  Check(!demand.load && demand.retain, "the outer radius retains without entering");

  observer.position.x = -6;
  demand = EvaluateWorldStreamDemand(observer, region);
  Check(!demand.retain, "a region outside the retain radius leaves demand");

  observer = Observer({0, 2000, 0});
  demand = EvaluateWorldStreamDemand(observer, region);
  Check(demand.load, "XZ observers ignore vertical separation");
  observer.axes = kWorldStreamXYZ;
  demand = EvaluateWorldStreamDemand(observer, region);
  Check(!demand.retain, "XYZ observers include vertical separation");

  observer = Observer({0, 0, 0}, 2, 4);
  observer.velocity = {100, 0, 0};
  observer.prediction_seconds = 1;
  observer.maximum_prediction_distance = 12;
  WorldStreamRegion ahead = Region(2, 10, -1, 11, 1);
  demand = EvaluateWorldStreamDemand(observer, ahead);
  Check(demand.load && demand.prediction_only, "the swept prediction requests ahead of motion");
  WorldStreamQuery query = BuildWorldStreamQuery(observer);
  Near(query.predicted.x, 12, "prediction is clamped to its maximum distance");
  Near(query.radius, 4, "broad-phase query uses the retain radius");

  observer.velocity = {1, 1000, 0};
  query = BuildWorldStreamQuery(observer);
  Near(query.predicted.x, 1, "ignored axes do not consume the prediction clamp");

  ahead.channels = 2;
  observer.channels = 1;
  demand = EvaluateWorldStreamDemand(observer, ahead);
  Check(!demand.load && !demand.retain, "channel masks isolate independent content domains");

  observer.velocity = {NAN, 0, 0};
  query = BuildWorldStreamQuery(observer);
  Near(query.predicted.x, query.origin.x, "non-finite velocity disables prediction safely");

  observer = Observer({NAN, 0, 0});
  demand = EvaluateWorldStreamDemand(observer, region);
  Check(!demand.load && !demand.retain, "invalid active-axis position disables observation");
  query = BuildWorldStreamQuery(observer);
  Check(query.channels == 0, "invalid active-axis query is disabled");
  observer = Observer({0, NAN, 0});
  demand = EvaluateWorldStreamDemand(observer, region);
  Check(demand.load, "invalid ignored-axis position does not disable observation");

  const f32 maximum = std::numeric_limits<f32>::max();
  observer = Observer({maximum, 0, 0}, 1, 2);
  observer.velocity = {maximum, maximum, 0};
  observer.prediction_seconds = maximum;
  observer.maximum_prediction_distance = maximum;
  query = BuildWorldStreamQuery(observer);
  Check(std::isfinite(query.predicted.x), "large finite prediction remains finite");
  demand = EvaluateWorldStreamDemand(observer, Region(3, -maximum, 0, -maximum, 1));
  Check(!demand.load, "overflow-scale distances do not spuriously load a region");

  const f32 large = 1.0e30f;
  const f32 adjacent = std::nextafter(large, maximum);
  observer = Observer({large, 0, 0}, 0, 0);
  observer.velocity = {adjacent - large, 0, 0};
  observer.prediction_seconds = 1;
  observer.maximum_prediction_distance = adjacent - large;
  demand = EvaluateWorldStreamDemand(observer, Region(4, adjacent, 0, adjacent, 0));
  Check(demand.load, "swept bounds remain conservative at large adjacent coordinates");
}

void TestLifecycleAndHysteresis() {
  WorldStreamPlan plan;
  WorldStreamObservation observer = Observer({0, 0, 0});
  const WorldStreamRegion region = Region(7, 5, -1, 6, 1);
  const WorldStreamTicket first = PrepareOne(plan, observer, region);
  MakeResident(plan, observer, region, first);
  Check(GetWorldStreamStats(plan).resident == 1, "completed content becomes resident");

  observer.position.x = -9;
  base::Vector<WorldStreamAction> actions;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&region, 1), {}, &actions);
  Check(actions.empty(), "resident content is stable in the hysteresis band");

  observer.position.x = -11;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&region, 1), {}, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kUnload, region.id) != nullptr,
        "resident content unloads beyond the retain distance");
  Check(!IsWorldStreamTicketCurrent(plan, first), "unload invalidates the old generation");

  observer.position = {};
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&region, 1), {}, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, region.id) == nullptr,
        "re-entry waits for old-generation retirement");
  Check(ApplyWorldStreamRetireResult(plan, first), "synchronous unload retirement is accepted");
  const WorldStreamTicket second = PrepareOne(plan, observer, region);
  Check(second.generation != first.generation, "re-entry receives a fresh generation");
  Check(!ApplyWorldStreamPrepareResult(plan, first, WorldStreamPrepareResult::kReady),
        "a late result from the old generation is rejected");

  observer.position.x = -20;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&region, 1), {}, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kCancel, region.id) != nullptr,
        "departing in-flight work emits cancellation");
  Check(!IsWorldStreamTicketCurrent(plan, second), "cancellation invalidates immediately");
  Check(ApplyWorldStreamRetireResult(plan, second), "canceled work retires after cleanup");
}

void TestBudgetsPriorityAndFairness() {
  WorldStreamPlan plan;
  const WorldStreamObservation observer = Observer({0, 0, 0}, 100, 110);
  WorldStreamRegion regions[] = {Region(3, 3, 0, 4, 1), Region(1, 1, 0, 2, 1),
                                 Region(2, 2, 0, 3, 1, 10)};
  WorldStreamFrameBudget budget;
  budget.maximum_prepare_starts = 2;
  budget.maximum_pending = 1;
  base::Vector<WorldStreamAction> actions;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), regions, budget, &actions);
  Check(actions.size() == 1 && actions[0].ticket.region == 2,
         "priority wins and the pending-work limit caps starts");
  if (actions.empty()) return;

  const WorldStreamTicket first = actions[0].ticket;
  ApplyWorldStreamPrepareResult(plan, first, WorldStreamPrepareResult::kReady);
  budget.maximum_pending = 3;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), regions, budget, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, 1) != nullptr,
        "nearest equal-priority region starts next");
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, 3) != nullptr,
        "remaining prepare capacity is used");

  for (const WorldStreamAction& action : actions) {
    if (action.kind == WorldStreamActionKind::kPrepare) {
      ApplyWorldStreamPrepareResult(plan, action.ticket, WorldStreamPrepareResult::kReady);
    }
    if (action.kind == WorldStreamActionKind::kCommit) {
      ApplyWorldStreamCommitResult(plan, action.ticket, WorldStreamCommitResult::kMoreWork);
    }
  }

  budget.maximum_prepare_starts = 0;
  budget.maximum_commit_steps = 1;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), regions, budget, &actions);
  Check(actions.size() == 1 && actions[0].kind == WorldStreamActionKind::kCommit,
         "commit work is bounded to one quantum");
  if (actions.empty()) return;
  const u64 served_first = actions[0].ticket.region;
  ApplyWorldStreamCommitResult(plan, actions[0].ticket, WorldStreamCommitResult::kMoreWork);
  AdvanceWorldStreaming(plan, std::span(&observer, 1), regions, budget, &actions);
  Check(actions.size() == 1 && actions[0].ticket.region != served_first,
        "round-robin age prevents a long commit from starving ready peers");

  WorldStreamPlan retry_priority;
  ConfigureWorldStreaming(retry_priority, {.retry_delay_ticks = 0});
  WorldStreamFrameBudget one_start;
  one_start.maximum_prepare_starts = 1;
  one_start.maximum_pending = 1;
  const WorldStreamRegion low = Region(20, 1, 0, 2, 1, 0);
  const WorldStreamRegion high = Region(21, 2, 0, 3, 1, 100);
  AdvanceWorldStreaming(retry_priority, std::span(&observer, 1), std::span(&low, 1), one_start,
                        &actions);
  if (actions.empty()) {
    Check(false, "retry setup emits a prepare");
    return;
  }
  ApplyWorldStreamPrepareResult(retry_priority, actions[0].ticket,
                                WorldStreamPrepareResult::kFailed);
  AdvanceWorldStreaming(retry_priority, std::span(&observer, 1), std::span(&low, 1), one_start,
                        &actions);
  const WorldStreamAction* cleanup = FindAction(actions, WorldStreamActionKind::kCancel, low.id);
  Check(cleanup != nullptr, "failed preparation requests cleanup before retry");
  if (!cleanup) return;
  ApplyWorldStreamRetireResult(retry_priority, cleanup->ticket);
  const WorldStreamRegion retry_candidates[] = {low, high};
  AdvanceWorldStreaming(retry_priority, std::span(&observer, 1), retry_candidates, one_start,
                        &actions);
  Check(actions.size() == 1 && actions[0].ticket.region == high.id,
         "a low-priority retry cannot starve fresh high-priority demand");

  WorldStreamPlan large_distance;
  const f32 farther = 1.0e30f;
  const f32 nearer = std::nextafter(farther, 0.0f);
  const WorldStreamObservation maximum_range =
      Observer({0, 0, 0}, std::numeric_limits<f32>::max(), std::numeric_limits<f32>::max());
  const WorldStreamRegion distant[] = {Region(1, farther, 0, farther, 1),
                                       Region(2, nearer, 0, nearer, 1)};
  AdvanceWorldStreaming(large_distance, std::span(&maximum_range, 1), distant, one_start,
                        &actions);
  Check(actions.size() == 1 && actions[0].ticket.region == 2,
        "large finite distances retain nearest-first priority ordering");

  WorldStreamPlan continuation;
  const WorldStreamRegion long_commit = Region(90, 50, 0, 51, 1);
  const WorldStreamRegion fresh_commit = Region(1, 1, 0, 2, 1);
  WorldStreamTicket continuation_ticket = PrepareOne(continuation, observer, long_commit);
  ApplyWorldStreamPrepareResult(continuation, continuation_ticket,
                                WorldStreamPrepareResult::kReady);
  AdvanceWorldStreaming(continuation, std::span(&observer, 1), std::span(&long_commit, 1), {},
                        &actions);
  const WorldStreamAction* initial_commit =
      FindAction(actions, WorldStreamActionKind::kCommit, long_commit.id);
  Check(initial_commit != nullptr, "continuation setup emits a commit");
  if (!initial_commit) return;
  ApplyWorldStreamCommitResult(continuation, initial_commit->ticket,
                               WorldStreamCommitResult::kMoreWork);
  const WorldStreamRegion commit_candidates[] = {long_commit, fresh_commit};
  WorldStreamFrameBudget admit_fresh;
  admit_fresh.maximum_commit_steps = 0;
  admit_fresh.maximum_pending = 2;
  AdvanceWorldStreaming(continuation, std::span(&observer, 1), commit_candidates, admit_fresh,
                        &actions);
  const WorldStreamAction* fresh_prepare =
      FindAction(actions, WorldStreamActionKind::kPrepare, fresh_commit.id);
  Check(fresh_prepare != nullptr, "fresh work is admitted beside a continuation");
  if (!fresh_prepare) return;
  ApplyWorldStreamPrepareResult(continuation, fresh_prepare->ticket,
                                WorldStreamPrepareResult::kReady);
  WorldStreamFrameBudget one_commit;
  one_commit.maximum_prepare_starts = 0;
  one_commit.maximum_commit_steps = 1;
  one_commit.maximum_pending = 2;
  AdvanceWorldStreaming(continuation, std::span(&observer, 1), commit_candidates, one_commit,
                        &actions);
  Check(actions.size() == 1 && actions[0].ticket.region == long_commit.id,
        "fresh ready work cannot starve an older commit continuation");

  WorldStreamPlan retry_fairness;
  ConfigureWorldStreaming(retry_fairness, {.retry_delay_ticks = 0});
  const WorldStreamRegion repeated_failure = Region(30, 1, 0, 2, 1);
  const WorldStreamRegion waiting_fresh = Region(31, 2, 0, 3, 1);
  WorldStreamTicket failure_ticket = PrepareOne(retry_fairness, observer, repeated_failure);
  ApplyWorldStreamPrepareResult(retry_fairness, failure_ticket,
                                WorldStreamPrepareResult::kFailed);
  const WorldStreamRegion fairness_candidates[] = {repeated_failure, waiting_fresh};
  AdvanceWorldStreaming(retry_fairness, std::span(&observer, 1), fairness_candidates, one_start,
                        &actions);
  const WorldStreamAction* first_cleanup =
      FindAction(actions, WorldStreamActionKind::kCancel, repeated_failure.id);
  Check(first_cleanup != nullptr, "retry fairness setup emits cleanup");
  if (!first_cleanup) return;
  ApplyWorldStreamRetireResult(retry_fairness, first_cleanup->ticket);
  AdvanceWorldStreaming(retry_fairness, std::span(&observer, 1), fairness_candidates, one_start,
                        &actions);
  Check(actions.size() == 1 && actions[0].ticket.region == repeated_failure.id,
        "an older retry gets its next fair prepare turn");
  if (actions.empty()) return;
  failure_ticket = actions[0].ticket;
  ApplyWorldStreamPrepareResult(retry_fairness, failure_ticket,
                                WorldStreamPrepareResult::kFailed);
  AdvanceWorldStreaming(retry_fairness, std::span(&observer, 1), fairness_candidates, one_start,
                        &actions);
  const WorldStreamAction* second_cleanup =
      FindAction(actions, WorldStreamActionKind::kCancel, repeated_failure.id);
  Check(second_cleanup != nullptr, "repeated failure emits another cleanup");
  if (!second_cleanup) return;
  ApplyWorldStreamRetireResult(retry_fairness, second_cleanup->ticket);
  AdvanceWorldStreaming(retry_fairness, std::span(&observer, 1), fairness_candidates, one_start,
                        &actions);
  Check(actions.size() == 1 && actions[0].ticket.region == waiting_fresh.id,
        "a repeatedly failing retry cannot starve older fresh demand");
}

base::Vector<u64> PrepareOrder(const WorldStreamRegion* regions, size_t count) {
  WorldStreamPlan plan;
  const WorldStreamObservation observer = Observer({0, 0, 0}, 100, 110);
  WorldStreamFrameBudget budget;
  budget.maximum_prepare_starts = 10;
  budget.maximum_pending = 10;
  base::Vector<WorldStreamAction> actions;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(regions, count), budget, &actions);
  base::Vector<u64> order;
  for (const WorldStreamAction& action : actions) order.push_back(action.ticket.region);
  return order;
}

void TestDeterminismAndMultipleObservers() {
  WorldStreamRegion forward[] = {Region(9, 9, 0, 10, 1), Region(2, 2, 0, 3, 1),
                                 Region(5, 5, 0, 6, 1)};
  WorldStreamRegion reverse[] = {forward[2], forward[1], forward[0]};
  Check(PrepareOrder(forward, 3) == PrepareOrder(reverse, 3),
        "candidate permutation does not change deterministic action order");

  WorldStreamPlan plan;
  WorldStreamObservation observers[] = {Observer({-100, 0, 0}, 5, 8), Observer({100, 0, 0}, 5, 8)};
  WorldStreamRegion regions[] = {Region(1, -100, 0, -99, 1), Region(2, 99, 0, 100, 1),
                                 Region(3, 0, 0, 1, 1)};
  WorldStreamFrameBudget budget;
  budget.maximum_prepare_starts = 3;
  budget.maximum_pending = 3;
  base::Vector<WorldStreamAction> actions;
  AdvanceWorldStreaming(plan, observers, regions, budget, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, 1) != nullptr &&
            FindAction(actions, WorldStreamActionKind::kPrepare, 2) != nullptr,
        "multiple observers contribute union demand");
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, 3) == nullptr,
        "regions outside every observer remain absent");

  WorldStreamPlan cleanup;
  WorldStreamFrameBudget cleanup_budget;
  cleanup_budget.maximum_prepare_starts = 3;
  cleanup_budget.maximum_pending = 3;
  const WorldStreamObservation broad = Observer({0, 0, 0}, 100, 110);
  AdvanceWorldStreaming(cleanup, std::span(&broad, 1), reverse, cleanup_budget, &actions);
  const WorldStreamObservation departed = Observer({1000, 0, 1000}, 1, 2);
  AdvanceWorldStreaming(cleanup, std::span(&departed, 1), reverse, cleanup_budget, &actions);
  Check(actions.size() == 3 && actions[0].ticket.region == 2 && actions[1].ticket.region == 5 &&
            actions[2].ticket.region == 9,
        "cancellation order is stable by region id");
}

void TestRetryResetAndGather() {
  WorldStreamPlan plan;
  ConfigureWorldStreaming(plan, {.retry_delay_ticks = 2});
  const WorldStreamObservation observer = Observer({0, 0, 0});
  const WorldStreamRegion region = Region(4, 1, 0, 2, 1);
  const WorldStreamTicket failed = PrepareOne(plan, observer, region);
  ApplyWorldStreamPrepareResult(plan, failed, WorldStreamPrepareResult::kFailed);

  base::Vector<WorldStreamAction> actions;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&region, 1), {}, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, region.id) == nullptr,
         "failed work observes its retry delay");
  const WorldStreamAction* failed_cleanup =
      FindAction(actions, WorldStreamActionKind::kCancel, region.id);
  Check(failed_cleanup != nullptr, "failed work is cleaned before its retry delay");
  if (!failed_cleanup) return;
  ApplyWorldStreamRetireResult(plan, failed_cleanup->ticket);
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&region, 1), {}, &actions);
  const WorldStreamAction* retry = FindAction(actions, WorldStreamActionKind::kPrepare, region.id);
  Check(retry && retry->ticket.generation != failed.generation,
        "retry starts with a new generation after the delay");
  const WorldStreamTicket retry_ticket = retry ? retry->ticket : WorldStreamTicket{};

  ResetWorldStreaming(plan, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kCancel, region.id) != nullptr,
        "reset emits cleanup for nonresident work");
  Check(GetWorldStreamStats(plan).retiring == 1, "reset waits for shell cleanup acknowledgement");
  Check(ApplyWorldStreamRetireResult(plan, retry_ticket), "reset retirement is acknowledged");
  Check(GetWorldStreamStats(plan).retiring == 0, "acknowledged reset clears plan state");

  rx::ecs::World world;
  const rx::ecs::Entity entity = world.Create();
  Transform transform;
  transform.position[0] = 3;
  transform.position[1] = 4;
  transform.position[2] = 5;
  world.Add(entity, transform);
  world.Add(entity, WorldStreamObserver{.velocity = {1, 2, 3},
                                        .load_distance = 20,
                                        .retain_distance = 30,
                                        .prediction_seconds = 1,
                                        .maximum_prediction_distance = 10,
                                        .channels = 7,
                                        .axes = kWorldStreamXZ});
  base::Vector<WorldStreamObservation> gathered;
  GatherWorldStreamObservers(world, &gathered);
  Check(gathered.size() == 1 && gathered[0].position.x == 3 && gathered[0].velocity.z == 3 &&
            gathered[0].channels == 7,
        "ECS observer gathering produces plain planning values");

  const rx::ecs::Entity child = world.Create();
  world.Add(child, Transform{});
  world.Add(child, WorldStreamObserver{.load_distance = 10, .retain_distance = 20});
  world.Add(child, Parent{entity});
  GatherWorldStreamObservers(world, &gathered);
  Check(gathered.size() == 1, "parent-local transforms are not mistaken for world observers");
}

void TestRetirementMetadataAndPendingPayloads() {
  WorldStreamPlan plan;
  const WorldStreamObservation observer = Observer({0, 0, 0}, 100, 110);
  WorldStreamRegion original = Region(10, 1, 0, 2, 1);
  const WorldStreamTicket old = PrepareOne(plan, observer, original);

  WorldStreamRegion moved = Region(10, 20, 0, 21, 1);
  base::Vector<WorldStreamAction> actions;
  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&moved, 1), {}, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kCancel, moved.id) != nullptr,
        "identity-affecting metadata changes retire the old payload");
  Check(!ApplyWorldStreamPrepareResult(plan, old, WorldStreamPrepareResult::kReady),
        "metadata retirement rejects the old preparation result");

  AdvanceWorldStreaming(plan, std::span(&observer, 1), std::span(&moved, 1), {}, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, moved.id) == nullptr,
        "replacement does not overlap a retiring payload");
  Check(ApplyWorldStreamRetireResult(plan, old), "metadata cleanup retirement is accepted");
  const WorldStreamTicket replacement = PrepareOne(plan, observer, moved);
  Check(replacement.generation != old.generation, "metadata replacement uses a new generation");

  WorldStreamPlan bounded;
  WorldStreamRegion regions[] = {Region(1, 1, 0, 2, 1), Region(2, 2, 0, 3, 1)};
  WorldStreamFrameBudget budget;
  budget.maximum_prepare_starts = 2;
  budget.maximum_commit_steps = 0;
  budget.maximum_pending = 1;
  AdvanceWorldStreaming(bounded, std::span(&observer, 1), regions, budget, &actions);
  Check(actions.size() == 1, "pending cap admits only one preparation");
  if (actions.empty()) return;
  ApplyWorldStreamPrepareResult(bounded, actions[0].ticket, WorldStreamPrepareResult::kReady);
  AdvanceWorldStreaming(bounded, std::span(&observer, 1), regions, budget, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, 2) == nullptr,
        "prepared payloads continue to consume the pending cap");

  WorldStreamPlan failed_metadata;
  ConfigureWorldStreaming(failed_metadata, {.retry_delay_ticks = 100});
  const WorldStreamTicket failed = PrepareOne(failed_metadata, observer, original);
  ApplyWorldStreamPrepareResult(failed_metadata, failed, WorldStreamPrepareResult::kFailed);
  AdvanceWorldStreaming(failed_metadata, std::span(&observer, 1), std::span(&moved, 1), {},
                        &actions);
  const WorldStreamAction* metadata_cleanup =
      FindAction(actions, WorldStreamActionKind::kCancel, original.id);
  Check(metadata_cleanup != nullptr, "failed old metadata is cleaned before replacement");
  if (!metadata_cleanup) return;
  ApplyWorldStreamRetireResult(failed_metadata, metadata_cleanup->ticket);
  AdvanceWorldStreaming(failed_metadata, std::span(&observer, 1), std::span(&moved, 1), {},
                        &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, moved.id) != nullptr,
         "new metadata is not held behind an old generation's retry delay");
}

void TestFailuresCatalogRemovalAndUnloadAdmission() {
  const WorldStreamRegion region = Region(30, 5, 0, 6, 1);
  WorldStreamObservation observer = Observer({0, 0, 0}, 10, 15);
  base::Vector<WorldStreamAction> actions;

  WorldStreamPlan prepare_failure;
  ConfigureWorldStreaming(prepare_failure, {.retry_delay_ticks = 0});
  WorldStreamTicket ticket = PrepareOne(prepare_failure, observer, region);
  ApplyWorldStreamPrepareResult(prepare_failure, ticket, WorldStreamPrepareResult::kFailed);
  observer.position.x = -6;
  AdvanceWorldStreaming(prepare_failure, std::span(&observer, 1), std::span(&region, 1), {},
                        &actions);
  const WorldStreamAction* prepare_cleanup =
      FindAction(actions, WorldStreamActionKind::kCancel, region.id);
  Check(prepare_cleanup != nullptr, "prepare failure requests partial-payload cleanup");
  if (!prepare_cleanup) return;
  ApplyWorldStreamRetireResult(prepare_failure, prepare_cleanup->ticket);
  AdvanceWorldStreaming(prepare_failure, std::span(&observer, 1), std::span(&region, 1), {},
                        &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, region.id) != nullptr,
         "failed requested content retries in the retain-only band");

  WorldStreamPlan commit_failure;
  ConfigureWorldStreaming(commit_failure, {.retry_delay_ticks = 0});
  observer.position = {};
  ticket = PrepareOne(commit_failure, observer, region);
  ApplyWorldStreamPrepareResult(commit_failure, ticket, WorldStreamPrepareResult::kReady);
  AdvanceWorldStreaming(commit_failure, std::span(&observer, 1), std::span(&region, 1), {},
                        &actions);
  const WorldStreamAction* commit = FindAction(actions, WorldStreamActionKind::kCommit, region.id);
  Check(commit != nullptr, "prepared content reaches commit before failure test");
  if (commit) {
    ApplyWorldStreamCommitResult(commit_failure, commit->ticket, WorldStreamCommitResult::kFailed);
    ticket = commit->ticket;
  }
  observer.position.x = -6;
  AdvanceWorldStreaming(commit_failure, std::span(&observer, 1), std::span(&region, 1), {},
                        &actions);
  Check(FindAction(actions, WorldStreamActionKind::kCancel, region.id) != nullptr,
        "failed partial commit requests cleanup");
  ApplyWorldStreamRetireResult(commit_failure, ticket);
  AdvanceWorldStreaming(commit_failure, std::span(&observer, 1), std::span(&region, 1), {},
                        &actions);
  Check(FindAction(actions, WorldStreamActionKind::kPrepare, region.id) != nullptr,
        "commit failure restarts while retained");

  WorldStreamPlan removed_pending;
  observer.position = {};
  ticket = PrepareOne(removed_pending, observer, region);
  AdvanceWorldStreaming(removed_pending, std::span(&observer, 1), {}, {}, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kCancel, region.id) != nullptr,
        "catalog removal cancels pending content");
  ApplyWorldStreamRetireResult(removed_pending, ticket);

  WorldStreamPlan delayed_unload;
  ticket = PrepareOne(delayed_unload, observer, region);
  MakeResident(delayed_unload, observer, region, ticket);
  WorldStreamFrameBudget no_unload;
  no_unload.maximum_unloads = 0;
  AdvanceWorldStreaming(delayed_unload, std::span(&observer, 1), {}, no_unload, &actions);
  Check(actions.empty() && GetWorldStreamStats(delayed_unload).resident == 1 &&
            IsWorldStreamTicketCurrent(delayed_unload, ticket),
        "resident content stays current until an unload slot is admitted");
  AdvanceWorldStreaming(delayed_unload, std::span(&observer, 1), std::span(&region, 1), no_unload,
                        &actions);
  Check(actions.empty() && GetWorldStreamStats(delayed_unload).resident == 1,
        "re-entry before unload dispatch keeps the resident generation");
  AdvanceWorldStreaming(delayed_unload, std::span(&observer, 1), {}, {}, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kUnload, region.id) != nullptr,
        "catalog removal unloads resident content once admitted");

  WorldStreamPlan bounded_unloads;
  WorldStreamRegion regions[] = {Region(1, 1, 0, 2, 1), Region(2, 2, 0, 3, 1),
                                 Region(3, 3, 0, 4, 1)};
  WorldStreamFrameBudget fill;
  fill.maximum_prepare_starts = 3;
  fill.maximum_commit_steps = 3;
  fill.maximum_pending = 3;
  AdvanceWorldStreaming(bounded_unloads, std::span(&observer, 1), regions, fill, &actions);
  for (const WorldStreamAction& action : actions) {
    ApplyWorldStreamPrepareResult(bounded_unloads, action.ticket, WorldStreamPrepareResult::kReady);
  }
  AdvanceWorldStreaming(bounded_unloads, std::span(&observer, 1), regions, fill, &actions);
  for (const WorldStreamAction& action : actions) {
    ApplyWorldStreamCommitResult(bounded_unloads, action.ticket,
                                 WorldStreamCommitResult::kComplete);
  }
  WorldStreamFrameBudget retire;
  retire.maximum_unloads = 3;
  retire.maximum_pending = 2;
  AdvanceWorldStreaming(bounded_unloads, std::span(&observer, 1), {}, retire, &actions);
  const WorldStreamStats stats = GetWorldStreamStats(bounded_unloads);
  Check(actions.size() == 2 && stats.retiring == 2 && stats.resident == 1,
         "unload admission cannot exceed the pending payload cap");

  WorldStreamPlan cleanup_first;
  const WorldStreamRegion old_region = Region(40, 1, 0, 2, 1);
  ticket = PrepareOne(cleanup_first, observer, old_region);
  MakeResident(cleanup_first, observer, old_region, ticket);
  const WorldStreamRegion fresh_region = Region(41, 100, 0, 101, 1);
  const WorldStreamObservation moved_observer = Observer({100, 0, 0}, 10, 15);
  const WorldStreamRegion moved_candidates[] = {old_region, fresh_region};
  WorldStreamFrameBudget one_pending;
  one_pending.maximum_prepare_starts = 1;
  one_pending.maximum_unloads = 1;
  one_pending.maximum_pending = 1;
  AdvanceWorldStreaming(cleanup_first, std::span(&moved_observer, 1), moved_candidates,
                        one_pending, &actions);
  Check(FindAction(actions, WorldStreamActionKind::kUnload, old_region.id) != nullptr &&
            FindAction(actions, WorldStreamActionKind::kPrepare, fresh_region.id) == nullptr,
        "cleanup takes a saturated pending slot before continual fresh demand");

  WorldStreamPlan unload_fairness;
  const WorldStreamRegion low_id = Region(1, 0, 0, 1, 1);
  const WorldStreamRegion high_id = Region(100, 100, 0, 101, 1);
  const WorldStreamRegion resident_regions[] = {low_id, high_id};
  const WorldStreamObservation broad = Observer({0, 0, 0}, 200, 210);
  WorldStreamFrameBudget fill_two;
  fill_two.maximum_prepare_starts = 2;
  fill_two.maximum_commit_steps = 2;
  fill_two.maximum_pending = 2;
  AdvanceWorldStreaming(unload_fairness, std::span(&broad, 1), resident_regions, fill_two,
                        &actions);
  for (const WorldStreamAction& action : actions) {
    ApplyWorldStreamPrepareResult(unload_fairness, action.ticket,
                                  WorldStreamPrepareResult::kReady);
  }
  AdvanceWorldStreaming(unload_fairness, std::span(&broad, 1), resident_regions, fill_two,
                        &actions);
  for (const WorldStreamAction& action : actions) {
    ApplyWorldStreamCommitResult(unload_fairness, action.ticket,
                                 WorldStreamCommitResult::kComplete);
  }
  const WorldStreamObservation near_low = Observer({0, 0, 0}, 10, 15);
  WorldStreamFrameBudget hold_unloads;
  hold_unloads.maximum_prepare_starts = 0;
  hold_unloads.maximum_commit_steps = 0;
  hold_unloads.maximum_unloads = 0;
  hold_unloads.maximum_pending = 1;
  AdvanceWorldStreaming(unload_fairness, std::span(&near_low, 1), resident_regions, hold_unloads,
                        &actions);
  const WorldStreamObservation far_away = Observer({1000, 0, 0}, 1, 2);
  hold_unloads.maximum_unloads = 1;
  AdvanceWorldStreaming(unload_fairness, std::span(&far_away, 1), resident_regions, hold_unloads,
                        &actions);
  Check(actions.size() == 1 && actions[0].ticket.region == high_id.id,
        "the oldest unload request wins before a newly obsolete lower id");
}

void TestBoundedTraversalStress() {
  WorldStreamPlan plan;
  WorldStreamFrameBudget budget;
  budget.maximum_prepare_starts = 4;
  budget.maximum_commit_steps = 4;
  budget.maximum_unloads = 8;
  budget.maximum_pending = 8;
  base::Vector<WorldStreamAction> actions;

  for (i32 step = 0; step < 200; ++step) {
    const WorldStreamObservation observer = Observer({static_cast<f32>(step), 0, 0}, 4, 7);
    base::Vector<WorldStreamRegion> candidates;
    for (i32 x = step - 8; x <= step + 8; ++x) {
      candidates.push_back(
          Region(static_cast<u64>(x + 1000), static_cast<f32>(x), 0, static_cast<f32>(x + 1), 1));
    }
    AdvanceWorldStreaming(plan, std::span(&observer, 1), candidates, budget, &actions);
    for (const WorldStreamAction& action : actions) {
      if (action.kind == WorldStreamActionKind::kPrepare) {
        ApplyWorldStreamPrepareResult(plan, action.ticket, WorldStreamPrepareResult::kReady);
      } else if (action.kind == WorldStreamActionKind::kCommit) {
        ApplyWorldStreamCommitResult(plan, action.ticket, WorldStreamCommitResult::kComplete);
      } else if (action.kind == WorldStreamActionKind::kCancel ||
                 action.kind == WorldStreamActionKind::kUnload) {
        ApplyWorldStreamRetireResult(plan, action.ticket);
      }
    }
    const WorldStreamStats stats = GetWorldStreamStats(plan);
    Check(stats.resident + stats.preparing + stats.ready + stats.committing + stats.retiring +
                  stats.failed <=
              20,
          "continual traversal keeps tracked residency bounded");
  }
}

}  // namespace

int main() {
  TestDemandAndQueries();
  TestLifecycleAndHysteresis();
  TestBudgetsPriorityAndFairness();
  TestDeterminismAndMultipleObservers();
  TestRetryResetAndGather();
  TestRetirementMetadataAndPendingPayloads();
  TestFailuresCatalogRemovalAndUnloadAdmission();
  TestBoundedTraversalStress();

  if (failures != 0) {
    std::fprintf(stderr, "world_streaming_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("world_streaming_test: PASS\n");
  return 0;
}
