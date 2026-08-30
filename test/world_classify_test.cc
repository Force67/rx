// What the cook makes of one entity, asked without running a cook.
//
// The editor shows this per selection, because the split between an ECS entity
// and an instance-page row is otherwise invisible until somebody inspects the
// archive - and getting that split wrong has already shipped one bug in this
// module. A label that lies is worse than no label, so the predicate behind it
// is pinned here rather than left to be checked by baking things and looking.
#include "world/world_bake.h"

#include <cstdio>
#include <string>

#include "ecs/world.h"
#include "scene/components.h"

namespace {

using namespace rx::world;
using rx::f32;
using rx::u32;
using rx::u64;
using rx::ecs::Entity;
using rx::scene::Guid;
using rx::scene::Name;
using rx::scene::Parent;
using rx::scene::Renderable;
using rx::scene::SpawnedFrom;
using rx::scene::Transform;

int g_failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

WorldBakeOptions Options(f32 cell_size = 32.0f) {
  WorldBakeOptions options;
  options.cell_size = cell_size;
  return options;
}

Transform At(f32 x, f32 y, f32 z) {
  Transform transform;
  transform.position[0] = x;
  transform.position[1] = y;
  transform.position[2] = z;
  return transform;
}

void TestTheTwoRoles() {
  rx::ecs::World world;
  const WorldBakeOptions options = Options();

  // A transform and a mesh and nothing else: a page row, no ECS identity.
  const Entity scatter = world.Create();
  world.Add(scatter, At(5, 0, 5));
  world.Add(scatter, Renderable{});
  CHECK(ClassifyForBake(world, scatter, options).role == BakeRole::kInstance);

  // A Guid does not change that. SaveScene puts one on everything it writes, so
  // if it counted, nothing an editor produced would ever be a page row.
  const Entity saved_scatter = world.Create();
  world.Add(saved_scatter, At(6, 0, 5));
  world.Add(saved_scatter, Renderable{});
  world.Add(saved_scatter, Guid{1234});
  CHECK(ClassifyForBake(world, saved_scatter, options).role == BakeRole::kInstance);

  // Anything else carrying behavior makes it an entity.
  const Entity prop = world.Create();
  world.Add(prop, At(7, 0, 5));
  world.Add(prop, Renderable{});
  world.Add(prop, SpawnedFrom{7});
  CHECK(ClassifyForBake(world, prop, options).role == BakeRole::kEntity);

  // A transform alone is an entity: it is not the instance set.
  const Entity marker = world.Create();
  world.Add(marker, At(8, 0, 5));
  CHECK(ClassifyForBake(world, marker, options).role == BakeRole::kEntity);
}

void TestClassifyIgnoresWhatTheDropWouldRemove() {
  rx::ecs::World world;
  const WorldBakeOptions options = Options();

  // Name holds a std::string, so it cannot be baked and is dropped. The entity
  // is still an entity: classifying after the drop would quietly turn a named,
  // authored thing into a page row with no identity at all.
  const Entity named = world.Create();
  world.Add(named, At(3, 0, 3));
  world.Add(named, Renderable{});
  world.Add(named, Name{"lamp post"});

  const BakeVerdict verdict = ClassifyForBake(world, named, options);
  CHECK(verdict.role == BakeRole::kEntity);
  CHECK(verdict.dropped.size() == 1);
  CHECK(verdict.dropped.size() == 1 && verdict.dropped[0] == "Name");
}

void TestRefusals() {
  rx::ecs::World world;
  const WorldBakeOptions options = Options();

  const Entity parent = world.Create();
  world.Add(parent, At(0, 0, 0));

  // A Parent is an ecs handle, which no baked cell can carry.
  const Entity child = world.Create();
  world.Add(child, At(1, 0, 1));
  world.Add(child, Parent{parent});
  const BakeVerdict parented = ClassifyForBake(world, child, options);
  CHECK(parented.role == BakeRole::kRefused);
  CHECK(parented.refusal.find("Parent") != std::string::npos);

  // No Transform: the cook has nowhere to put it.
  const Entity placeless = world.Create();
  world.Add(placeless, Renderable{});
  const BakeVerdict nowhere = ClassifyForBake(world, placeless, options);
  CHECK(nowhere.role == BakeRole::kRefused);
  CHECK(!nowhere.has_cell);

  // Far enough out that its cell would alias onto another's.
  const Entity distant = world.Create();
  world.Add(distant, At(1e30f, 0, 0));
  CHECK(ClassifyForBake(world, distant, options).role == BakeRole::kRefused);
}

void TestCellAndBounds() {
  rx::ecs::World world;
  const Entity entity = world.Create();
  world.Add(entity, At(70, 3, 12));

  const BakeVerdict verdict = ClassifyForBake(world, entity, Options(32.0f));
  CHECK(verdict.has_cell);
  // x = 70 is in the third 32 m column, z = 12 in the first row.
  CHECK(verdict.cell_minimum.x == 64.0f && verdict.cell_maximum.x == 96.0f);
  CHECK(verdict.cell_minimum.z == 0.0f && verdict.cell_maximum.z == 32.0f);

  // The cell size is a cook setting, not scene data, so the same entity lands
  // somewhere else under different options. This is exactly why the editor
  // holds one set of options for both the label and the bake, and prints the
  // size it assumed.
  const BakeVerdict coarse = ClassifyForBake(world, entity, Options(64.0f));
  CHECK(coarse.has_cell);
  CHECK(coarse.cell_minimum.x == 64.0f && coarse.cell_maximum.x == 128.0f);
  CHECK(coarse.cell != verdict.cell);

  // Negative coordinates fold into the lattice rather than aliasing onto the
  // positive side.
  const Entity negative = world.Create();
  world.Add(negative, At(-5, 0, -40));
  const BakeVerdict below = ClassifyForBake(world, negative, Options(32.0f));
  CHECK(below.has_cell);
  CHECK(below.cell_minimum.x == -32.0f && below.cell_maximum.x == 0.0f);
  CHECK(below.cell_minimum.z == -64.0f && below.cell_maximum.z == -32.0f);
  CHECK(below.cell != verdict.cell);
}

void TestInstanceSetIsConfigurable() {
  rx::ecs::World world;
  const Entity entity = world.Create();
  world.Add(entity, At(1, 0, 1));
  world.Add(entity, Renderable{});
  world.Add(entity, SpawnedFrom{3});

  // Default set: Transform + Renderable, so the SpawnedFrom makes it an entity.
  CHECK(ClassifyForBake(world, entity, Options()).role == BakeRole::kEntity);

  // Widen the set and the same object becomes a page row instead.
  WorldBakeOptions wider = Options();
  wider.instance_components.push_back("Transform");
  wider.instance_components.push_back("Renderable");
  wider.instance_components.push_back("SpawnedFrom");
  CHECK(ClassifyForBake(world, entity, wider).role == BakeRole::kInstance);
}

}  // namespace

int main() {
  TestTheTwoRoles();
  TestClassifyIgnoresWhatTheDropWouldRemove();
  TestRefusals();
  TestCellAndBounds();
  TestInstanceSetIsConfigurable();
  if (g_failures) {
    std::fprintf(stderr, "world_classify_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::puts("world_classify_test: ok");
  return 0;
}
