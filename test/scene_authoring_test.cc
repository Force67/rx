// Headless tests for the .rxscene authoring passes that turn a declaration into
// engine state, focused on Rotation: the euler convention it promises, the
// save/load round trip that has to reproduce the file exactly, and the three
// things a rotation interacts with (anchors, grids, prefabs). No GPU: every
// builder takes a null renderer, which is the viewer's own --headless path.
// Exits non-zero on the first failure so it slots into ctest.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "asset/asset_database.h"
#include "asset/vfs.h"
#include "core/math.h"
#include "ecs/world.h"
#include "edit/hierarchy.h"
#include "edit/scene_io.h"
#include "scene/components.h"
#include "scene_authoring.h"

using namespace rx;

namespace {

int failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                 \
    }                                                             \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

namespace fs = std::filesystem;

fs::path WriteScene(const char* name, const std::string& body) {
  const fs::path path = fs::temp_directory_path() / name;
  std::ofstream out(path, std::ios::binary);
  out << "rxscene 1\n" << body;
  return path;
}

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

// The viewer's own pass order (Viewer::LoadRxScene), so what this test measures
// is what a render would draw. False when any of them refuses the file.
bool LoadAndBuild(ecs::World& world, asset::AssetDatabase& db, const fs::path& path) {
  std::string error;
  const std::string file = path.string();
  if (!edit::LoadScene(world, db, file, &error, /*strict=*/true)) {
    std::printf("load error: %s\n", error.c_str());
    return false;
  }
  if (!BuildSceneGrids(world, file, &error) || !BuildScenePrefabs(world, file, &error)) {
    std::printf("layout error: %s\n", error.c_str());
    return false;
  }
  BuildSceneRotations(world);
  if (!BuildSceneShapes(world, db, /*renderer=*/nullptr, &error) ||
      !BuildSceneModels(world, db, /*renderer=*/nullptr, file, &error) ||
      !BuildSceneAnchors(world, file, &error)) {
    std::printf("build error: %s\n", error.c_str());
    return false;
  }
  return true;
}

ecs::Entity FindByName(ecs::World& world, const std::string& name) {
  ecs::Entity found = ecs::kInvalidEntity;
  world.Each<scene::Name>([&](ecs::Entity entity, scene::Name& value) {
    if (value.value == name) found = entity;
  });
  return found;
}

Quat WorldRotation(ecs::World& world, ecs::Entity entity) {
  const scene::Transform t = edit::WorldTransform(world, entity);
  return {t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]};
}

// The convention the Rotation hint promises, pinned by the two cases that can
// tell it from every other one: which way a positive angle turns, and which
// order the three axes compose in.
void TestEulerConvention() {
  const fs::path path = WriteScene("rx_rotation_convention.rxscene", R"(
entity
Name.value = "Yawed"
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5
Rotation.euler = 0 90 0

entity
Name.value = "Yawed and pitched"
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5
Rotation.euler = 90 90 0
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  // Right-handed and y-up: +90 about y takes the entity's +z face onto +x.
  const Quat yaw = WorldRotation(world, FindByName(world, "Yawed"));
  const Vec3 forward = Rotate(yaw, {0, 0, 1});
  CHECK_NEAR(forward.x, 1.0f, 1e-5f);
  CHECK_NEAR(forward.y, 0.0f, 1e-5f);
  CHECK_NEAR(forward.z, 0.0f, 1e-5f);

  // Yaw THEN pitch about the entity's own axes: +y goes to +z under the pitch
  // and that +z to +x under the yaw. The other order would leave +y where it is
  // through the yaw and hand back +z, so this is what distinguishes them.
  const Quat both = WorldRotation(world, FindByName(world, "Yawed and pitched"));
  const Vec3 up = Rotate(both, {0, 1, 0});
  CHECK_NEAR(up.x, 1.0f, 1e-5f);
  CHECK_NEAR(up.y, 0.0f, 1e-5f);
  CHECK_NEAR(up.z, 0.0f, 1e-5f);

  // --validate's non_unit_rotation warns past 5%, and MakeFromQuat does not
  // normalize, so a euler that did not resolve to a unit quaternion would scale
  // every mesh it is written to.
  const f32 length = std::sqrt(yaw.x * yaw.x + yaw.y * yaw.y + yaw.z * yaw.z + yaw.w * yaw.w);
  CHECK_NEAR(length, 1.0f, 1e-5f);

  fs::remove(path);
}

// Write it, load it, save it, load THAT and save again: the two saved files have
// to be byte-identical, or a live edit of an authored scene rewrites content the
// author did not touch. The first save is what settles the random guids, so the
// comparison starts from it rather than from the source text.
void TestRoundTrip() {
  const fs::path path = WriteScene("rx_rotation_roundtrip.rxscene", R"(
entity
Name.value = "Plinth"
Transform.position = 0 0.5 0
Shape.kind = "box"
Shape.size = 1 0.5 1

entity
Name.value = "Tilted"
Rotation.euler = 12.5 -37 4
Anchor.target = "Plinth"
Anchor.mode = "on"
Shape.kind = "box"
Shape.size = 0.3 0.3 0.3
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World first;
  CHECK(LoadAndBuild(first, db, path));

  const fs::path saved_a = fs::temp_directory_path() / "rx_rotation_roundtrip_a.rxscene";
  const fs::path saved_b = fs::temp_directory_path() / "rx_rotation_roundtrip_b.rxscene";
  std::string error;
  CHECK(edit::SaveScene(first, saved_a.string(), &error));

  ecs::World second;
  CHECK(LoadAndBuild(second, db, saved_a));
  CHECK(edit::SaveScene(second, saved_b.string(), &error));
  CHECK(ReadFile(saved_a) == ReadFile(saved_b));

  // The source declaration survives alongside the value it resolved into: a
  // save that dropped Rotation would leave a bare quaternion no one can edit,
  // and one that re-applied it on load would turn the entity twice.
  CHECK(ReadFile(saved_a).find("Rotation.euler") != std::string::npos);
  const Quat before = WorldRotation(first, FindByName(first, "Tilted"));
  const Quat after = WorldRotation(second, FindByName(second, "Tilted"));
  CHECK_NEAR(before.x, after.x, 1e-6f);
  CHECK_NEAR(before.y, after.y, 1e-6f);
  CHECK_NEAR(before.z, after.z, 1e-6f);
  CHECK_NEAR(before.w, after.w, 1e-6f);

  fs::remove(path);
  fs::remove(saved_a);
  fs::remove(saved_b);
}

// An anchor stands one box on another by measuring both, and a turned box has a
// taller footprint than the one it was authored with. Standing it on the
// unrotated extent would bury a corner in the plinth.
void TestAnchorOnRotated() {
  const fs::path path = WriteScene("rx_rotation_anchor.rxscene", R"(
entity
Name.value = "Plinth"
Transform.position = 0 0.5 0
Shape.kind = "box"
Shape.size = 1 0.5 1

entity
Name.value = "Upright"
Anchor.target = "Plinth"
Anchor.mode = "on"
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5

entity
Name.value = "Cornered"
Rotation.euler = 0 0 45
Anchor.target = "Plinth"
Anchor.mode = "on"
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  // The plinth's top face is y = 1, so an axis-aligned half-extent 0.5 box
  // centres at 1.5 and the same box stood on one edge at 1 + 0.5 * sqrt(2).
  const scene::Transform* upright = world.Get<scene::Transform>(FindByName(world, "Upright"));
  CHECK_NEAR(upright->position[1], 1.5f, 1e-4f);
  const scene::Transform* cornered = world.Get<scene::Transform>(FindByName(world, "Cornered"));
  CHECK_NEAR(cornered->position[1], 1.0f + 0.5f * std::sqrt(2.0f), 1e-4f);
  // The two axes the mode does not stack along still centre on the target.
  CHECK_NEAR(cornered->position[0], 0.0f, 1e-4f);
  CHECK_NEAR(cornered->position[2], 0.0f, 1e-4f);

  fs::remove(path);
}

// A grid member is a child of its container, so a rotation on either composes
// the way a parent chain does: the container's turns the whole layout about its
// own origin, the member's turns only that cell.
void TestRotatedGrid() {
  const fs::path path = WriteScene("rx_rotation_grid.rxscene", R"(
entity
Name.value = "Row"
Transform.position = 0 0 0
Rotation.euler = 0 90 0
Grid.count = 2 1 1
Grid.step = 4 0 0

entity
Name.value = "First"
Grid.of = "Row"
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5

entity
Name.value = "Second"
Grid.of = "Row"
Rotation.euler = 0 45 0
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  // Cell 1 sits 4 along local +x, and the container's 90 degree yaw takes local
  // +x onto world -z.
  const scene::Transform second = edit::WorldTransform(world, FindByName(world, "Second"));
  CHECK_NEAR(second.position[0], 0.0f, 1e-4f);
  CHECK_NEAR(second.position[2], -4.0f, 1e-4f);
  // ... and the member's own 45 adds to the container's 90, so its +z face ends
  // up 135 degrees round.
  const Vec3 facing = Rotate(WorldRotation(world, FindByName(world, "Second")), {0, 0, 1});
  CHECK_NEAR(facing.x, std::sin(135.0f * 3.14159265f / 180.0f), 1e-4f);
  CHECK_NEAR(facing.z, std::cos(135.0f * 3.14159265f / 180.0f), 1e-4f);

  const Vec3 unrotated = Rotate(WorldRotation(world, FindByName(world, "First")), {0, 0, 1});
  CHECK_NEAR(unrotated.x, 1.0f, 1e-4f);

  fs::remove(path);
}

// The prefab override rule is per component, so an instance that says anything
// about Rotation owns its orientation and one that says nothing takes the
// prefab's, exactly like Shape and Surface.
void TestPrefabRotation() {
  const fs::path prefab = WriteScene("rx_rotation_prefab_cell.rxscene", R"(
entity
Name.value = "Cell"
Rotation.euler = 0 90 0
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5
)");
  const fs::path path = WriteScene("rx_rotation_prefab.rxscene",
                                   R"(
entity
Name.value = "Inherits"
Transform.position = 0 0 0
Prefab.path = "rx_rotation_prefab_cell.rxscene"

entity
Name.value = "Overrides"
Transform.position = 3 0 0
Rotation.euler = 0 180 0
Prefab.path = "rx_rotation_prefab_cell.rxscene"
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  const Vec3 inherited = Rotate(WorldRotation(world, FindByName(world, "Inherits")), {0, 0, 1});
  CHECK_NEAR(inherited.x, 1.0f, 1e-4f);
  const Vec3 overridden = Rotate(WorldRotation(world, FindByName(world, "Overrides")), {0, 0, 1});
  CHECK_NEAR(overridden.z, -1.0f, 1e-4f);

  fs::remove(prefab);
  fs::remove(path);
}

}  // namespace

int main() {
  RegisterSceneComponents();
  TestEulerConvention();
  TestRoundTrip();
  TestAnchorOnRotated();
  TestRotatedGrid();
  TestPrefabRotation();
  if (failures == 0) std::printf("scene_authoring_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
