// rx::placement acceptance: the ordered-dither pattern's quality guarantees,
// density-program evaluation, and the CPU reference generator that mirrors
// the GPU pipeline - determinism, footprint spacing, layered dithering
// (same-footprint layers never collide), exclusion masks, local stability
// under WorldData edits, and streaming ring bookkeeping. No GPU.

#include <cmath>
#include <cstdio>
#include <cstring>

#include "placement/density_program.h"
#include "placement/ecotope.h"
#include "placement/placement.h"
#include "placement/placement_math.h"
#include "placement/placement_pattern.h"
#include "placement/world_data.h"

namespace {

using namespace rx;
using namespace rx::placement;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "placement_test: FAIL: %s\n", message);
  ++failures;
}

void Near(f32 actual, f32 expected, const char* message, f32 epsilon = 1e-3f) {
  if (std::fabs(actual - expected) <= epsilon) return;
  std::fprintf(stderr, "placement_test: FAIL: %s (got %.4f, expected %.4f)\n", message,
               actual, expected);
  ++failures;
}

f32 ToroidalDistance(f32 ax, f32 ay, f32 bx, f32 by) {
  f32 dx = std::fabs(ax - bx);
  f32 dy = std::fabs(ay - by);
  dx = std::min(dx, 1.0f - dx);
  dy = std::min(dy, 1.0f - dy);
  return std::sqrt(dx * dx + dy * dy);
}

// The two pattern-generator rules: even threshold coverage at every prefix,
// and maximal spacing between successive points (any prefix is a usable
// point set on its own).
void TestPattern() {
  for (u32 i = 0; i < kPatternPoints; ++i) {
    f32 x = kPatternXY[i * 2 + 0];
    f32 y = kPatternXY[i * 2 + 1];
    Check(x >= 0.0f && x < 1.0f && y >= 0.0f && y < 1.0f, "pattern point in unit tile");
  }

  for (u32 prefix = 2; prefix <= kPatternPoints; prefix *= 2) {
    f32 min_dist = 10.0f;
    for (u32 i = 0; i < prefix; ++i) {
      for (u32 j = i + 1; j < prefix; ++j) {
        min_dist = std::min(
            min_dist, ToroidalDistance(kPatternXY[i * 2], kPatternXY[i * 2 + 1],
                                       kPatternXY[j * 2], kPatternXY[j * 2 + 1]));
      }
    }
    f32 ideal = std::sqrt(1.0f / static_cast<f32>(prefix));
    Check(min_dist >= 0.5f * ideal, "pattern prefix keeps half the ideal spacing");
  }

  // Even coverage: every 4x4 cell holds its fair share of the full pattern.
  u32 cells[16] = {};
  for (u32 i = 0; i < kPatternPoints; ++i) {
    u32 cx = std::min(static_cast<u32>(kPatternXY[i * 2] * 4.0f), 3u);
    u32 cy = std::min(static_cast<u32>(kPatternXY[i * 2 + 1] * 4.0f), 3u);
    ++cells[cy * 4 + cx];
  }
  for (u32 c = 0; c < 16; ++c) {
    Check(cells[c] >= 8 && cells[c] <= 24, "pattern cell coverage within bounds");
  }
}

void TestDensityProgram() {
  WorldData world(0.0f, 0.0f, 128.0f, 64);
  u32 half = world.AddMap("half", 0.5f);
  u32 ramp = world.AddMap("ramp");
  world.Generate(ramp, [](f32 x, f32) { return x / 128.0f; });

  DensityProgram constant;
  constant.Const(0.25f);
  Near(EvalDensityProgram(constant.ops(), world, 10.0f, 10.0f), 0.25f, "const pushes");

  DensityProgram product;
  product.Map(half).Map(ramp).Mul();
  Near(EvalDensityProgram(product.ops(), world, 64.0f, 32.0f), 0.5f * (64.0f / 128.0f),
       "map product", 0.02f);

  DensityProgram inverted;
  inverted.Map(half).OneMinus();
  Near(EvalDensityProgram(inverted.ops(), world, 5.0f, 5.0f), 0.5f, "one minus");

  DensityProgram banded;
  banded.Map(ramp).Range(0.25f, 0.75f);
  Near(EvalDensityProgram(banded.ops(), world, 0.0f, 0.0f), 0.0f, "range low", 0.02f);
  Near(EvalDensityProgram(banded.ops(), world, 127.0f, 0.0f), 1.0f, "range high", 0.02f);

  DensityProgram saturated;
  saturated.Const(3.0f);
  Near(EvalDensityProgram(saturated.ops(), world, 0.0f, 0.0f), 1.0f, "result saturates");

  // The noise op agrees with the shared helper and stays in [0,1].
  DensityProgram noisy;
  noisy.Noise(16.0f, 7.0f);
  Near(EvalDensityProgram(noisy.ops(), world, 33.0f, 71.0f),
       ValueNoise(33.0f, 71.0f, 16.0f, 7u), "noise matches shared helper");
}

PlacementSystem MakeForestSystem(const WorldData* world, u32 height, u32 forest, u32 road,
                                 f32 jitter = 0.0f) {
  PlacementConfig config;
  config.jitter = jitter;
  PlacementSystem system(world, height, config);
  Ecotope forest_ecotope;
  forest_ecotope.name = "forest";
  {
    PlacementLayer pine;
    pine.name = "pine";
    pine.mesh = 1;
    pine.footprint = 4.0f;
    pine.density.Map(forest).Map(road).OneMinus().Mul().Const(0.5f).Mul();
    pine.scale_min = 0.8f;
    pine.scale_max = 1.3f;
    forest_ecotope.layers.push_back(pine);
  }
  {
    PlacementLayer fir;
    fir.name = "fir";
    fir.mesh = 2;
    fir.footprint = 4.0f;
    fir.density.Map(forest).Map(road).OneMinus().Mul().Const(0.5f).Mul();
    forest_ecotope.layers.push_back(fir);
  }
  {
    PlacementLayer grass;
    grass.name = "grass";
    grass.mesh = 3;
    grass.footprint = 1.0f;
    grass.density.Const(1.0f);
    forest_ecotope.layers.push_back(grass);
  }
  system.AddEcotope(forest_ecotope);
  system.Compile();
  return system;
}

void TestCompile() {
  WorldData world(0.0f, 0.0f, 512.0f, 256);
  u32 height = world.AddMap("height");
  u32 forest = world.AddMap("forest", 1.0f);
  u32 road = world.AddMap("road");
  PlacementSystem system = MakeForestSystem(&world, height, forest, road);

  Check(system.stacks().size() == 2, "same-footprint layers share a stack");
  Check(system.stacks()[0].layer_count == 2, "tree stack holds both layers");
  Check(system.stacks()[0].tile_size == 64.0f, "tile size is footprint x 16");
  Check(system.stacks()[1].layer_count == 1, "grass stack holds one layer");
}

void TestDeterminismAndSpacing() {
  WorldData world(0.0f, 0.0f, 512.0f, 256);
  u32 height = world.AddMap("height");
  world.Generate(height, [](f32 x, f32 z) {
    return 4.0f * std::sin(x * 0.02f) + 3.0f * std::cos(z * 0.015f);
  });
  u32 forest = world.AddMap("forest", 1.0f);
  u32 road = world.AddMap("road");
  PlacementSystem system = MakeForestSystem(&world, height, forest, road);

  TileKey tile{0, 1, 2};
  base::Vector<PlacedInstance> a;
  base::Vector<PlacedInstance> b;
  system.EmitTileCpu(tile, a);
  system.EmitTileCpu(tile, b);
  Check(a.size() == b.size() && !a.empty(), "regeneration reproduces the count");
  bool identical = a.size() == b.size();
  for (u32 i = 0; identical && i < a.size(); ++i) {
    identical = std::memcmp(&a[i], &b[i], sizeof(PlacedInstance)) == 0;
  }
  Check(identical, "regeneration reproduces instances bit for bit");

  // Full cumulative density -> every pattern point lands; spacing follows the
  // footprint (no jitter in this system).
  Check(a.size() == kPatternPointCount, "full density fills the pattern");
  const PlacementStack& stack = system.stacks()[0];
  f32 min_dist = 1e9f;
  for (u32 i = 0; i < a.size(); ++i) {
    for (u32 j = i + 1; j < a.size(); ++j) {
      f32 dx = a[i].transform.m[12] - a[j].transform.m[12];
      f32 dz = a[i].transform.m[14] - a[j].transform.m[14];
      min_dist = std::min(min_dist, std::sqrt(dx * dx + dz * dz));
    }
  }
  Check(min_dist >= 0.6f * stack.footprint, "instances keep footprint spacing");

  // Layered dithering: the two tree layers split the pattern without ever
  // claiming the same sample point.
  u32 pine_count = 0;
  u32 fir_count = 0;
  bool point_collision = false;
  bool seen[kPatternPointCount] = {};
  for (const PlacedInstance& instance : a) {
    if (instance.layer == 0) ++pine_count;
    if (instance.layer == 1) ++fir_count;
    if (seen[instance.point]) point_collision = true;
    seen[instance.point] = true;
  }
  Check(!point_collision, "layered dithering never doubles a sample point");
  Check(pine_count >= 100 && pine_count <= 156, "pine takes roughly half the stack");
  Check(fir_count == kPatternPointCount - pine_count, "fir takes the rest");

  // Instances sit on the height map with unit-ish basis vectors scaled by the
  // per-instance scale.
  for (const PlacedInstance& instance : a) {
    f32 x = instance.transform.m[12];
    f32 z = instance.transform.m[14];
    Near(instance.transform.m[13], world.Sample(height, x, z), "instance sits on ground",
         0.01f);
    f32 sx = std::sqrt(instance.transform.m[0] * instance.transform.m[0] +
                       instance.transform.m[1] * instance.transform.m[1] +
                       instance.transform.m[2] * instance.transform.m[2]);
    Check(sx >= 0.79f && sx <= 1.31f, "instance scale within layer range");
  }
}

void TestExclusionMask() {
  WorldData world(0.0f, 0.0f, 512.0f, 256);
  u32 height = world.AddMap("height");
  u32 forest = world.AddMap("forest", 1.0f);
  u32 road = world.AddMap("road");
  // A road strip through the middle of tile (0,0) of the tree stack.
  world.Generate(road, [](f32 x, f32) { return (x >= 24.0f && x < 40.0f) ? 1.0f : 0.0f; });
  PlacementSystem system = MakeForestSystem(&world, height, forest, road);

  base::Vector<PlacedInstance> instances;
  system.EmitTileCpu({0, 0, 0}, instances);
  Check(!instances.empty(), "forest still populates off the road");
  for (const PlacedInstance& instance : instances) {
    f32 x = instance.transform.m[12];
    Check(x < 25.0f || x > 39.0f, "no tree on the road");
  }
}

void TestLocalStability() {
  WorldData world(0.0f, 0.0f, 1024.0f, 256);
  u32 height = world.AddMap("height");
  u32 forest = world.AddMap("forest", 1.0f);
  u32 road = world.AddMap("road");
  PlacementSystem system = MakeForestSystem(&world, height, forest, road);

  TileKey near_tile{0, 0, 0};
  TileKey far_tile{0, 8, 8};
  base::Vector<PlacedInstance> far_before;
  system.EmitTileCpu(far_tile, far_before);
  base::Vector<PlacedInstance> near_before;
  system.EmitTileCpu(near_tile, near_before);

  // A painted clearing inside the near tile: near changes, far is untouched.
  world.PaintDisc(forest, 32.0f, 32.0f, 24.0f, 0.0f);
  base::Vector<PlacedInstance> near_after;
  system.EmitTileCpu(near_tile, near_after);
  base::Vector<PlacedInstance> far_after;
  system.EmitTileCpu(far_tile, far_after);

  Check(near_after.size() < near_before.size(), "clearing removes trees locally");
  bool far_identical = far_before.size() == far_after.size();
  for (u32 i = 0; far_identical && i < far_before.size(); ++i) {
    far_identical = std::memcmp(&far_before[i], &far_after[i], sizeof(PlacedInstance)) == 0;
  }
  Check(far_identical, "distant tile unaffected by the edit");

  // Invalidation only marks tiles whose evaluation can see the edit. Both
  // tiles sit inside the viewer's ring so distance cannot evict either; only
  // the edited one may regenerate.
  TileKey neighbor_tile{0, 3, 3};
  system.MarkLive(near_tile);
  system.MarkLive(neighbor_tile);
  system.InvalidateRegion(8.0f, 8.0f, 56.0f, 56.0f);
  system.Update({128.0f, 0.0f, 128.0f});
  bool near_evicted = false;
  bool neighbor_evicted = false;
  for (const TileKey& key : system.evicted()) {
    if (key == near_tile) near_evicted = true;
    if (key == neighbor_tile) neighbor_evicted = true;
  }
  Check(near_evicted, "edited tile is invalidated");
  Check(!neighbor_evicted, "unedited tile is not invalidated");
}

void TestStreamingRing() {
  WorldData world(0.0f, 0.0f, 2048.0f, 256);
  u32 height = world.AddMap("height");
  u32 forest = world.AddMap("forest", 1.0f);
  u32 road = world.AddMap("road");
  PlacementSystem system = MakeForestSystem(&world, height, forest, road);

  system.Update({100.0f, 0.0f, 100.0f});
  Check(!system.pending().empty(), "viewer ring requests tiles");
  // Drain the ring (each update hands out at most max_jobs_per_update).
  for (u32 round = 0; round < 32 && !system.pending().empty(); ++round) {
    for (const TileKey& key : system.pending()) system.MarkInFlight(key);
    system.Update({100.0f, 0.0f, 100.0f});
  }
  Check(system.pending().empty(), "ring saturates once tiles are in flight");

  // Moving far away evicts live tiles.
  base::Vector<TileKey> all;
  system.Update({100.0f, 0.0f, 100.0f});
  system.MarkLive({0, 0, 0});
  system.Update({1900.0f, 0.0f, 1900.0f});
  bool evicted = false;
  for (const TileKey& key : system.evicted()) {
    if (key == TileKey{0, 0, 0}) evicted = true;
  }
  Check(evicted, "leaving the ring evicts the tile");
}

}  // namespace

int main() {
  TestPattern();
  TestDensityProgram();
  TestCompile();
  TestDeterminismAndSpacing();
  TestExclusionMask();
  TestLocalStability();
  TestStreamingRing();
  if (failures == 0) std::printf("placement_test: all checks passed\n");
  return failures;
}
