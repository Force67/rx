// Headless tests for the .rxscene authoring passes that turn a declaration into
// engine state. Rotation: the euler convention it promises, the save/load round
// trip that has to reproduce the file exactly, and the three things a rotation
// interacts with (anchors, grids, prefabs). Stretch: that it composes with
// Shape.size and with a PREFAB's Shape (the case it is a component of its own
// for), that the mesh key tells two proportions apart, and that the bounds
// anchors and grids measure are the STRETCHED ones. No GPU: every builder takes
// a null renderer, which is the viewer's own --headless path, so the shading
// half of the stretch (the normals) is proved by render instead. Exits non-zero
// on the first failure so it slots into ctest.

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
  if (!BuildSceneShapes(world, db, /*renderer=*/nullptr, file, &error) ||
      !BuildSceneModels(world, db, /*renderer=*/nullptr, file, &error) ||
      !BuildSceneAnchors(world, file, &error)) {
    std::printf("build error: %s\n", error.c_str());
    return false;
  }
  return true;
}

// The same pass order, kept quiet and handing back the refusal: for the files
// that are supposed to fail, where the message is the thing under test.
std::string LoadAndBuildError(ecs::World& world, asset::AssetDatabase& db, const fs::path& path) {
  std::string error;
  const std::string file = path.string();
  if (!edit::LoadScene(world, db, file, &error, /*strict=*/true)) return error;
  if (!BuildSceneGrids(world, file, &error) || !BuildScenePrefabs(world, file, &error)) {
    return error;
  }
  BuildSceneRotations(world);
  if (!BuildSceneShapes(world, db, /*renderer=*/nullptr, file, &error) ||
      !BuildSceneModels(world, db, /*renderer=*/nullptr, file, &error) ||
      !BuildSceneAnchors(world, file, &error)) {
    return error;
  }
  return {};
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

// Anchor.offset is what makes a ground plane usable as a target: centring is
// right for a plant room on a tower and useless for a street, where everything
// stands on one floor at a different place on it. The height stays derived,
// which is the whole point - these three land on the ground without the file
// naming any of their heights, and go on landing on it when a Stretch changes
// one of them.
void TestAnchorOffset() {
  const fs::path path = WriteScene("rx_anchor_offset.rxscene", R"(
entity
Name.value = "Ground"
Transform.position = 0 0 0
Shape.kind = "plane"
Shape.size = 20 0 20

entity
Name.value = "Short"
Anchor.target = "Ground"
Anchor.mode = "on"
Anchor.offset = -6 0 2
Shape.kind = "box"
Shape.size = 1 1.5 1

entity
Name.value = "Tall"
Anchor.target = "Ground"
Anchor.mode = "on"
Anchor.offset = 6 0 -3
Shape.kind = "box"
Shape.size = 1 1.5 1
Stretch.scale = 1 4 1
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  // Each sits on its own half height, which is the number the file never wrote:
  // 1.5 as authored, and 1.5 * 4 once the stretch is baked in.
  const scene::Transform* shorter = world.Get<scene::Transform>(FindByName(world, "Short"));
  CHECK_NEAR(shorter->position[1], 1.5f, 1e-4f);
  const scene::Transform* taller = world.Get<scene::Transform>(FindByName(world, "Tall"));
  CHECK_NEAR(taller->position[1], 6.0f, 1e-4f);
  // Across the other two axes the offset displaces from the target's centre,
  // rather than the centring winning and stacking both on one spot.
  CHECK_NEAR(shorter->position[0], -6.0f, 1e-4f);
  CHECK_NEAR(shorter->position[2], 2.0f, 1e-4f);
  CHECK_NEAR(taller->position[0], 6.0f, 1e-4f);
  CHECK_NEAR(taller->position[2], -3.0f, 1e-4f);

  // The offset is authored INPUT, so a save writes it beside the position it
  // helped solve and the reload re-derives the same place. Were it folded into
  // Transform.position instead, every round trip would walk the object another
  // offset along - which is the bug the anchor replaces rather than offsets to
  // avoid in the first place.
  const fs::path saved = fs::temp_directory_path() / "rx_anchor_offset_saved.rxscene";
  std::string error;
  CHECK(edit::SaveScene(world, saved.string(), &error));
  CHECK(ReadFile(saved).find("Anchor.offset") != std::string::npos);

  ecs::World reloaded;
  CHECK(LoadAndBuild(reloaded, db, saved));
  const scene::Transform* again = reloaded.Get<scene::Transform>(FindByName(reloaded, "Tall"));
  CHECK_NEAR(again->position[0], 6.0f, 1e-4f);
  CHECK_NEAR(again->position[1], 6.0f, 1e-4f);
  CHECK_NEAR(again->position[2], -3.0f, 1e-4f);

  fs::remove(path);
  fs::remove(saved);
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

// Stretch.scale multiplies the built geometry per axis, so a box may reach the
// same extents through either component. The two are not the same MESH, though:
// the key has to keep them apart, or the second entity draws the first's
// geometry.
void TestStretchComposesWithSize() {
  const fs::path path = WriteScene("rx_stretch_compose.rxscene", R"(
entity
Name.value = "Sized"
Shape.kind = "box"
Shape.size = 1 0.5 1.5

entity
Name.value = "Stretched"
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5
Stretch.scale = 2 1 3

entity
Name.value = "Twin"
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5
Stretch.scale = 2 1 3

entity
Name.value = "Unstretched"
Shape.kind = "box"
Shape.size = 0.5 0.5 0.5
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  const SceneBounds* sized = world.Get<SceneBounds>(FindByName(world, "Sized"));
  const SceneBounds* stretched = world.Get<SceneBounds>(FindByName(world, "Stretched"));
  CHECK(sized && stretched);
  for (int axis = 0; axis < 3; ++axis) {
    CHECK_NEAR(stretched->min[axis], sized->min[axis], 1e-5f);
    CHECK_NEAR(stretched->max[axis], sized->max[axis], 1e-5f);
  }

  // Same extents reached a different way is a different mesh (the vertices are
  // baked, so nothing downstream can tell the two apart later), the same
  // numbers in the same components is one shared mesh, and dropping the stretch
  // has to leave the key somewhere else again.
  const asset::AssetId by_size = world.Get<scene::Renderable>(FindByName(world, "Sized"))->mesh;
  const asset::AssetId by_stretch =
      world.Get<scene::Renderable>(FindByName(world, "Stretched"))->mesh;
  const asset::AssetId twin = world.Get<scene::Renderable>(FindByName(world, "Twin"))->mesh;
  const asset::AssetId plain =
      world.Get<scene::Renderable>(FindByName(world, "Unstretched"))->mesh;
  CHECK(by_size.hash != by_stretch.hash);
  CHECK(by_stretch.hash == twin.hash);
  CHECK(by_stretch.hash != plain.hash);

  fs::remove(path);
}

// A prefab of more than one piece has to stretch WHOLE, or the only shape that
// survives being proportioned is a single box - which is what kept every
// authored building a rectangle. Each part's offset scales, so the crown stays
// on the shaft, and each part's geometry scales, so it widens with it.
void TestPrefabStretchReachesParts() {
  const fs::path prefab = WriteScene("rx_stretch_parts_cell.rxscene", R"(
entity
Name.value = "Podium"
Shape.kind = "box"
Shape.size = 2 1 2

entity
Name.value = "Shaft"
Transform.position = 0 5 0
Shape.kind = "box"
Shape.size = 1 4 1

entity
Name.value = "Crown"
Transform.position = 0 10 0
Shape.kind = "box"
Shape.size = 1.5 1 1.5
Stretch.scale = 1 0.5 1
)");
  const fs::path path = WriteScene("rx_stretch_parts.rxscene", R"(
entity
Name.value = "Plain"
Transform.position = 0 0 0
Prefab.path = "rx_stretch_parts_cell.rxscene"

entity
Name.value = "Tall"
Transform.position = 30 0 0
Stretch.scale = 0.5 2 0.5
Prefab.path = "rx_stretch_parts_cell.rxscene"
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  // Every part of the plain instance is where the prefab put it.
  const scene::Transform* plain_shaft = world.Get<scene::Transform>(FindByName(world, "Shaft"));
  CHECK(plain_shaft != nullptr);

  // The parts are Transient children named by the prefab, and both instances
  // expand the same names, so collect by name and sort by the height each
  // landed at: the tall instance's shaft and crown are the far ones.
  f32 heights[8] = {};
  u32 count = 0;
  world.Each<scene::Name, scene::Transform>(
      [&](ecs::Entity, scene::Name& name, scene::Transform& transform) {
        if (name.value == "Crown" && count < 8) heights[count++] = transform.position[1];
      });
  CHECK(count == 2);
  // One crown stayed at 10, the other doubled to 20 with the y stretch. Without
  // the offset scaling it would have stayed at 10 and sat inside the shaft.
  const f32 low = std::min(heights[0], heights[1]);
  const f32 high = std::max(heights[0], heights[1]);
  CHECK_NEAR(low, 10.0f, 1e-4f);
  CHECK_NEAR(high, 20.0f, 1e-4f);

  // And the part's own geometry took the stretch, multiplied into the one the
  // prefab authored for it (1 0.5 1 * 0.5 2 0.5), so the crown is proportioned
  // like the building rather than left at its authored size.
  bool checked_crown = false;
  world.Each<scene::Name, SceneStretch>(
      [&](ecs::Entity entity, scene::Name& name, SceneStretch& stretch) {
        const scene::Transform* at = world.Get<scene::Transform>(entity);
        if (name.value != "Crown" || !at || at->position[1] < 15.0f) return;
        CHECK_NEAR(stretch.scale[0], 0.5f, 1e-6f);
        CHECK_NEAR(stretch.scale[1], 1.0f, 1e-6f);
        CHECK_NEAR(stretch.scale[2], 0.5f, 1e-6f);
        checked_crown = true;
      });
  CHECK(checked_crown);

  fs::remove(prefab);
  fs::remove(path);
}

// A turned part cannot take a per-axis stretch: that is a shear, and nothing in
// the transform path carries one. Refused by name rather than rendered wrong.
void TestPrefabStretchRefusesShear() {
  const fs::path prefab = WriteScene("rx_stretch_shear_cell.rxscene", R"(
entity
Name.value = "Base"
Shape.kind = "box"
Shape.size = 1 1 1

entity
Name.value = "Fin"
Transform.position = 0 2 0
Rotation.euler = 0 30 0
Shape.kind = "box"
Shape.size = 1 0.2 0.4
)");
  const fs::path path = WriteScene("rx_stretch_shear.rxscene", R"(
entity
Name.value = "Sheared"
Transform.position = 0 0 0
Stretch.scale = 3 1 1
Prefab.path = "rx_stretch_shear_cell.rxscene"
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  const std::string error = LoadAndBuildError(world, db, path);
  CHECK(error.find("Fin") != std::string::npos);
  CHECK(error.find("shear") != std::string::npos);

  // A UNIFORM stretch of the same prefab is a similarity, so it is allowed.
  const fs::path uniform = WriteScene("rx_stretch_shear_uniform.rxscene", R"(
entity
Name.value = "Scaled"
Transform.position = 0 0 0
Stretch.scale = 3 3 3
Prefab.path = "rx_stretch_shear_cell.rxscene"
)");
  ecs::World fine;
  CHECK(LoadAndBuild(fine, db, uniform));

  fs::remove(prefab);
  fs::remove(path);
  fs::remove(uniform);
}

// The reason Stretch is a component of its own: prefab merge is per component,
// so an instance that stretches a prefab keeps the prefab's Shape and Surface
// and only changes its proportions. As a Shape prop this could not be said at
// all - authoring any part of Shape replaces the prefab's whole Shape, which
// silently loses the kind and size and draws a default box.
void TestPrefabStretch() {
  const fs::path prefab = WriteScene("rx_stretch_prefab_cell.rxscene", R"(
entity
Name.value = "Tower"
Shape.kind = "box"
Shape.size = 1 4 1
Surface.base_color = 0.2 0.4 0.6
Surface.roughness = 0.35
)");
  const fs::path path = WriteScene("rx_stretch_prefab.rxscene", R"(
entity
Name.value = "Plain"
Transform.position = 0 0 0
Prefab.path = "rx_stretch_prefab_cell.rxscene"

entity
Name.value = "Wide"
Transform.position = 6 0 0
Stretch.scale = 2 0.5 1
Prefab.path = "rx_stretch_prefab_cell.rxscene"

entity
Name.value = "Slim"
Transform.position = 12 0 0
Stretch.scale = 0.5 1.5 1
Prefab.path = "rx_stretch_prefab_cell.rxscene"
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  const f32 expected[3][3] = {{1, 4, 1}, {2, 2, 1}, {0.5f, 6, 1}};
  const char* names[3] = {"Plain", "Wide", "Slim"};
  for (int i = 0; i < 3; ++i) {
    const ecs::Entity entity = FindByName(world, names[i]);
    // The prefab's geometry, at this instance's proportions: the Shape itself
    // is untouched, which is what a second instance of the same prefab and a
    // re-save both depend on.
    const SceneShape* shape = world.Get<SceneShape>(entity);
    CHECK(shape && shape->kind == "box");
    CHECK_NEAR(shape->size[1], 4.0f, 1e-6f);
    const SceneBounds* bounds = world.Get<SceneBounds>(entity);
    CHECK(bounds != nullptr);
    for (int axis = 0; axis < 3; ++axis) CHECK_NEAR(bounds->max[axis], expected[i][axis], 1e-5f);
    // ... and the facade rides along, which is the other half of "the instance
    // owns only what it authored".
    const SceneSurface* surface = world.Get<SceneSurface>(entity);
    CHECK(surface && std::abs(surface->base_color[2] - 0.6f) < 1e-6f);
  }

  // Three proportions of one prefab are three meshes, never a shared one.
  const asset::AssetId plain = world.Get<scene::Renderable>(FindByName(world, "Plain"))->mesh;
  const asset::AssetId wide = world.Get<scene::Renderable>(FindByName(world, "Wide"))->mesh;
  const asset::AssetId slim = world.Get<scene::Renderable>(FindByName(world, "Slim"))->mesh;
  CHECK(plain.hash != wide.hash);
  CHECK(wide.hash != slim.hash);
  CHECK(plain.hash != slim.hash);

  // A save of the expanded scene reloads onto the same proportions: the
  // instance's Stretch survives beside the Shape the expansion left on it.
  const fs::path saved = fs::temp_directory_path() / "rx_stretch_prefab_saved.rxscene";
  std::string error;
  CHECK(edit::SaveScene(world, saved.string(), &error));
  CHECK(ReadFile(saved).find("Stretch.scale = 2 0.5 1") != std::string::npos);
  ecs::World reloaded;
  CHECK(LoadAndBuild(reloaded, db, saved));
  const SceneBounds* before = world.Get<SceneBounds>(FindByName(world, "Wide"));
  const SceneBounds* after = reloaded.Get<SceneBounds>(FindByName(reloaded, "Wide"));
  CHECK(before && after);
  for (int axis = 0; axis < 3; ++axis) CHECK_NEAR(before->max[axis], after->max[axis], 1e-6f);

  fs::remove(saved);
  fs::remove(prefab);
  fs::remove(path);
}

// An anchor measures built geometry, and after this change the built geometry is
// the stretched geometry. Nothing in BuildSceneAnchors knows about stretch, so
// this is the check that the bake really did reach SceneBounds rather than
// leaving the placement to be worked out from the authored Shape.size.
void TestAnchorOnStretched() {
  const fs::path path = WriteScene("rx_stretch_anchor.rxscene", R"(
entity
Name.value = "Plinth"
Transform.position = 0 1 0
Shape.kind = "box"
Shape.size = 1 0.5 1
Stretch.scale = 1 2 1

entity
Name.value = "Column"
Anchor.target = "Plinth"
Anchor.mode = "on"
Shape.kind = "box"
Shape.size = 0.25 0.25 0.25
Stretch.scale = 1 3 1

entity
Name.value = "Beside"
Anchor.target = "Plinth"
Anchor.mode = "right"
Shape.kind = "sphere"
Shape.size = 0.5 0.5 0.5
Stretch.scale = 4 1 1
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  // The plinth is half extent 0.5 stretched to 1, centred at y = 1, so its top
  // face is y = 2; the column is half extent 0.25 stretched to 0.75.
  const scene::Transform* column = world.Get<scene::Transform>(FindByName(world, "Column"));
  CHECK_NEAR(column->position[1], 2.75f, 1e-4f);
  // ... and an ellipsoid stands off the plinth's +x face by its own stretched
  // radius, which is the case Shape.size cannot author at all.
  const scene::Transform* beside = world.Get<scene::Transform>(FindByName(world, "Beside"));
  CHECK_NEAR(beside->position[0], 1.0f + 2.0f, 1e-3f);
  CHECK_NEAR(beside->position[1], 1.0f, 1e-3f);

  fs::remove(path);
}

// A grid steps by Grid.step whatever its members are made of, so a stretched
// cell lands on the same coordinate an unstretched one would. The point of the
// check is that the bake did not move the geometry off its own origin.
void TestStretchedGrid() {
  const fs::path path = WriteScene("rx_stretch_grid.rxscene", R"(
entity
Name.value = "Row"
Transform.position = 0 0 0
Grid.count = 2 1 1
Grid.step = 3 0 0

entity
Name.value = "First"
Grid.of = "Row"
Shape.kind = "cylinder"
Shape.size = 0.5 1 0
Stretch.scale = 1 1 2

entity
Name.value = "Second"
Grid.of = "Row"
Shape.kind = "cylinder"
Shape.size = 0.5 1 0
Stretch.scale = 1 1 2
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  CHECK(LoadAndBuild(world, db, path));

  const scene::Transform second = edit::WorldTransform(world, FindByName(world, "Second"));
  CHECK_NEAR(second.position[0], 3.0f, 1e-4f);

  // An oval column: 0.5 across x and 1 across z, which no Shape.size can say
  // for a cylinder, and centred on its own origin the way the grid assumes.
  const SceneBounds* bounds = world.Get<SceneBounds>(FindByName(world, "First"));
  CHECK(bounds != nullptr);
  CHECK_NEAR(bounds->max[0], 0.5f, 1e-4f);
  CHECK_NEAR(bounds->max[2], 1.0f, 1e-4f);
  CHECK_NEAR(bounds->min[0], -0.5f, 1e-4f);
  CHECK_NEAR(bounds->min[2], -1.0f, 1e-4f);

  fs::remove(path);
}

// Zero would make the normal bake divide by it and hand the whole mesh nans, so
// the load says no and names the line rather than uploading the wreckage.
void TestDegenerateStretchFailsTheLoad() {
  const fs::path path = WriteScene("rx_stretch_degenerate.rxscene", R"(
entity
Name.value = "Flattened"
Shape.kind = "sphere"
Shape.size = 0.5 0.5 0.5
Stretch.scale = 1 0 1
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World world;
  const std::string error = LoadAndBuildError(world, db, path);
  CHECK(error.find("rx_stretch_degenerate.rxscene:7:") != std::string::npos);
  CHECK(error.find("Stretch.scale") != std::string::npos);

  fs::remove(path);
}

// Same round trip the rotation test makes: a saved scene reloads and re-saves
// byte-identically, so a live edit of a stretched scene does not rewrite the
// proportions the author set.
void TestStretchRoundTrip() {
  const fs::path path = WriteScene("rx_stretch_roundtrip.rxscene", R"(
entity
Name.value = "Tower"
Transform.position = 0 6 0
Shape.kind = "box"
Shape.size = 1 1 1
Stretch.scale = 2.5 6 1.75
)");
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World first;
  CHECK(LoadAndBuild(first, db, path));

  const fs::path saved_a = fs::temp_directory_path() / "rx_stretch_roundtrip_a.rxscene";
  const fs::path saved_b = fs::temp_directory_path() / "rx_stretch_roundtrip_b.rxscene";
  std::string error;
  CHECK(edit::SaveScene(first, saved_a.string(), &error));
  ecs::World second;
  CHECK(LoadAndBuild(second, db, saved_a));
  CHECK(edit::SaveScene(second, saved_b.string(), &error));
  CHECK(ReadFile(saved_a) == ReadFile(saved_b));
  CHECK(ReadFile(saved_a).find("Stretch.scale = 2.5 6 1.75") != std::string::npos);

  const SceneBounds* before = first.Get<SceneBounds>(FindByName(first, "Tower"));
  const SceneBounds* after = second.Get<SceneBounds>(FindByName(second, "Tower"));
  CHECK(before && after);
  for (int axis = 0; axis < 3; ++axis) CHECK_NEAR(before->max[axis], after->max[axis], 1e-6f);

  fs::remove(path);
  fs::remove(saved_a);
  fs::remove(saved_b);
}

}  // namespace

int main() {
  RegisterSceneComponents();
  TestEulerConvention();
  TestRoundTrip();
  TestAnchorOnRotated();
  TestAnchorOffset();
  TestRotatedGrid();
  TestPrefabRotation();
  TestStretchComposesWithSize();
  TestPrefabStretch();
  TestAnchorOnStretched();
  TestPrefabStretchReachesParts();
  TestPrefabStretchRefusesShear();
  TestStretchedGrid();
  TestDegenerateStretchFailsTheLoad();
  TestStretchRoundTrip();
  if (failures == 0) std::printf("scene_authoring_test: all checks passed\n");
  return failures == 0 ? 0 : 1;
}
