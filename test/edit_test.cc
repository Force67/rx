// Headless tests for the engine/edit foundations: reflection, scene
// serialization round-trip, undo/redo (including across entity recreation) and
// world-transform composition. No GPU, no game assets. Exits non-zero on the
// first failure so it slots into ctest.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "asset/asset_database.h"
#include "asset/asset_id.h"
#include "asset/vfs.h"
#include "core/math.h"
#include "ecs/world.h"
#include "edit/hierarchy.h"
#include "edit/reflect.h"
#include "edit/scene_io.h"
#include "edit/selection.h"
#include "edit/undo.h"
#include "scene/components.h"

using namespace rx;
using namespace rx::edit;

namespace {

int failures = 0;

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
      ++failures;                                                    \
    }                                                                \
  } while (0)

#define CHECK_NEAR(a, b, eps) CHECK(std::abs((a) - (b)) <= (eps))

// A component exercising every PropType.
struct TestAll {
  bool b = false;
  i32 i = 0;
  u32 u = 0;
  rx::u64 uu = 0;
  f32 f = 0;
  f32 v2[2] = {};
  Vec3 v3{};
  f32 v4[4] = {};
  Quat q{};
  f32 col[4] = {};
  std::string s;
  asset::AssetId aid;
  ecs::Entity ent;
};

void RegisterTestComponent() {
  ReflectComponent<TestAll>("TestAll")
      .Prop("b", &TestAll::b)
      .Prop("i", &TestAll::i)
      .Prop("u", &TestAll::u)
      .Prop("uu", &TestAll::uu)
      .Prop("f", &TestAll::f)
      .Range(-1.f, 1.f)
      .Prop("v2", &TestAll::v2)
      .Prop("v3", &TestAll::v3)
      .Prop("v4", &TestAll::v4, PropType::kVec4)
      .Prop("q", &TestAll::q)
      .Prop("col", &TestAll::col, PropType::kColor)
      .Prop("s", &TestAll::s)
      .Prop("aid", &TestAll::aid)
      .Prop("ent", &TestAll::ent);
}

const PropDesc* FindProp(const ComponentDesc& c, std::string_view name) {
  for (u32 i = 0; i < c.prop_count; ++i)
    if (name == c.props[i].name) return &c.props[i];
  return nullptr;
}

void TestReflection() {
  RegisterTestComponent();

  const ComponentDesc* desc = FindComponentByName("TestAll");
  CHECK(desc != nullptr);
  if (!desc) return;
  CHECK(desc->prop_count == 13);
  CHECK(FindComponent(desc->id) == desc);

  // Deduced types.
  CHECK(FindProp(*desc, "b")->type == PropType::kBool);
  CHECK(FindProp(*desc, "i")->type == PropType::kI32);
  CHECK(FindProp(*desc, "u")->type == PropType::kU32);
  CHECK(FindProp(*desc, "uu")->type == PropType::kU64);
  CHECK(FindProp(*desc, "f")->type == PropType::kF32);
  CHECK(FindProp(*desc, "v2")->type == PropType::kVec2);
  CHECK(FindProp(*desc, "v3")->type == PropType::kVec3);
  CHECK(FindProp(*desc, "v4")->type == PropType::kVec4);
  CHECK(FindProp(*desc, "q")->type == PropType::kQuat);
  CHECK(FindProp(*desc, "col")->type == PropType::kColor);
  CHECK(FindProp(*desc, "s")->type == PropType::kString);
  CHECK(FindProp(*desc, "aid")->type == PropType::kAssetId);
  CHECK(FindProp(*desc, "ent")->type == PropType::kEntity);
  CHECK_NEAR(FindProp(*desc, "f")->min, -1.f, 1e-6f);
  CHECK_NEAR(FindProp(*desc, "f")->max, 1.f, 1e-6f);

  ecs::World world;
  ecs::Entity e = world.Create();
  ecs::Entity ref = world.Create();
  CHECK(AddComponentByDesc(world, e, *desc));

  auto set = [&](const char* n, PropValue v) { SetProp(world, e, *desc, *FindProp(*desc, n), v); };
  auto get = [&](const char* n) {
    PropValue v;
    GetProp(world, e, *desc, *FindProp(*desc, n), &v);
    return v;
  };

  set("b", PropValue::Bool(true));
  set("i", PropValue::I32(-42));
  set("u", PropValue::U32(42));
  set("uu", PropValue::U64(0x1122334455667788ull));
  set("f", PropValue::F32(0.5f));
  set("v2", PropValue::Vec2(1, 2));
  set("v3", PropValue::Vec3(3, 4, 5));
  set("v4", PropValue::Vec4(6, 7, 8, 9));
  set("q", PropValue::Quat(0, 0, 0, 1));
  set("col", PropValue::Color(0.1f, 0.2f, 0.3f, 0.4f));
  set("s", PropValue::String("hello world"));
  set("aid", PropValue::AssetIdV(0xdeadbeef));
  set("ent", PropValue::EntityV(ref));

  CHECK(get("b").b == true);
  CHECK(get("i").i == -42);
  CHECK(get("u").u == 42);
  CHECK(get("uu").u == 0x1122334455667788ull);
  CHECK_NEAR(get("f").f[0], 0.5f, 1e-6f);
  CHECK_NEAR(get("v2").f[1], 2.f, 1e-6f);
  CHECK_NEAR(get("v3").f[2], 5.f, 1e-6f);
  CHECK_NEAR(get("v4").f[3], 9.f, 1e-6f);
  CHECK_NEAR(get("q").f[3], 1.f, 1e-6f);
  CHECK_NEAR(get("col").f[0], 0.1f, 1e-6f);
  CHECK(get("s").s == "hello world");
  CHECK(get("aid").u == 0xdeadbeef);
  CHECK(get("ent").e == ref);

  // ComponentsOn reports the component; removal drops it.
  auto on = ComponentsOn(world, e);
  CHECK(std::find(on.begin(), on.end(), desc) != on.end());
  CHECK(RemoveComponentByDesc(world, e, *desc));
  CHECK(!world.HasRaw(e, desc->id));
}

// Compares one entity's reflected state across two worlds; kEntity props are
// compared by the referenced entity's guid.
bool DeepEqual(ecs::World& wa, ecs::Entity a, ecs::World& wb, ecs::Entity b) {
  auto ca = ComponentsOn(wa, a);
  auto cb = ComponentsOn(wb, b);
  if (ca.size() != cb.size()) return false;
  for (const ComponentDesc* comp : ca) {
    if (!wb.HasRaw(b, comp->id)) return false;
    for (u32 i = 0; i < comp->prop_count; ++i) {
      const PropDesc& p = comp->props[i];
      PropValue va, vb;
      GetProp(wa, a, *comp, p, &va);
      GetProp(wb, b, *comp, p, &vb);
      if (p.type == PropType::kEntity) {
        rx::u64 ga = va.e ? EnsureGuid(wa, va.e) : 0;
        rx::u64 gb = vb.e ? EnsureGuid(wb, vb.e) : 0;
        if (ga != gb) return false;
      } else if (p.type == PropType::kString) {
        if (va.s != vb.s) return false;
      } else if (p.type == PropType::kAssetId || p.type == PropType::kU64 ||
                 p.type == PropType::kU32) {
        if (va.u != vb.u) return false;
      } else if (p.type == PropType::kI32) {
        if (va.i != vb.i) return false;
      } else if (p.type == PropType::kBool) {
        if (va.b != vb.b) return false;
      } else {
        for (int k = 0; k < 4; ++k)
          if (std::abs(va.f[k] - vb.f[k]) > 1e-5f) return false;
      }
    }
  }
  return true;
}

void TestSceneRoundTrip() {
  namespace fs = std::filesystem;
  fs::path path = fs::temp_directory_path() / "rx_edit_roundtrip.rxscene";

  // A parent with a named, renderable child that references the parent.
  ecs::World src;
  ecs::Entity parent = src.Create();
  src.Add(parent, scene::Transform{{10, 0, 0}, {0, 0, 0, 1}, 2.0f});
  src.Add(parent, scene::Name{"Parent"});

  ecs::Entity child = src.Create();
  src.Add(child, scene::Transform{{1, 0, 0}, {0, 0, 0, 1}, 1.0f});
  src.Add(child, scene::Name{"Child"});
  src.Add(child, scene::Parent{parent});

  // One renderable with a known path (exercises the path branch) and one with a
  // raw hash only (exercises the fallback).
  asset::AssetId with_path = asset::MakeAssetId(asset::NormalizePath("meshes/foo.mesh"));
  asset::RecordAssetPath(with_path, asset::NormalizePath("meshes/foo.mesh"));
  src.Add(child, scene::Renderable{with_path});

  ecs::Entity loose = src.Create();
  src.Add(loose, scene::Transform{});
  src.Add(loose, scene::Renderable{asset::AssetId{0x12345678}});  // no recorded path
  src.Add(loose, scene::Hidden{});

  std::string err;
  CHECK(SaveScene(src, path.string(), &err));
  if (!err.empty()) std::printf("save error: %s\n", err.c_str());

  // Reload into a fresh world.
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);
  ecs::World dst;
  err.clear();
  CHECK(LoadScene(dst, db, path.string(), &err));
  if (!err.empty()) std::printf("load error: %s\n", err.c_str());

  // Same number of identity entities and per-entity deep equality (matched by guid).
  size_t src_count = 0;
  src.Each<scene::Guid>([&](ecs::Entity, scene::Guid&) { ++src_count; });
  size_t dst_count = 0;
  dst.Each<scene::Guid>([&](ecs::Entity, scene::Guid&) { ++dst_count; });
  CHECK(src_count == 3);
  CHECK(dst_count == 3);

  bool all_matched = true;
  src.Each<scene::Guid>([&](ecs::Entity se, scene::Guid& g) {
    ecs::Entity de = FindByGuid(dst, g.value);
    if (!de || !DeepEqual(src, se, dst, de)) all_matched = false;
  });
  CHECK(all_matched);

  // The renderable path resolved back to the same asset id.
  ecs::Entity dchild = FindByGuid(dst, src.Get<scene::Guid>(child)->value);
  CHECK(dchild);
  if (dchild) CHECK(dst.Get<scene::Renderable>(dchild)->mesh == with_path);

  fs::remove(path);
}

void TestUndo() {
  ecs::World world;
  UndoStack stack;

  const ComponentDesc* transform = FindComponentByName("Transform");
  const PropDesc* scale = FindProp(*transform, "scale");
  const ComponentDesc* name = FindComponentByName("Name");

  // Create via command.
  ecs::Entity e;
  stack.Push(world, MakeCreateEntity({{transform, {}}}, &e));
  CHECK(world.IsAlive(e));
  CHECK(world.Has<scene::Transform>(e));
  rx::u64 guid = world.Get<scene::Guid>(e)->value;

  // Set a property, undo, redo.
  stack.Push(world, MakeSetProp(world, e, *transform, *scale, PropValue::F32(5.0f)));
  CHECK_NEAR(world.Get<scene::Transform>(e)->scale, 5.0f, 1e-6f);
  CHECK(stack.Undo(world));
  CHECK_NEAR(world.Get<scene::Transform>(e)->scale, 1.0f, 1e-6f);
  CHECK(stack.Redo(world));
  CHECK_NEAR(world.Get<scene::Transform>(e)->scale, 5.0f, 1e-6f);

  // Grouped add-component + set-name.
  stack.BeginGroup("Rename & tag");
  stack.Push(world, MakeAddComponent(world, e, *name));
  const PropDesc* name_value = FindProp(*name, "value");
  stack.Push(world, MakeSetProp(world, e, *name, *name_value, PropValue::String("Hero")));
  stack.EndGroup();
  CHECK(world.Has<scene::Name>(e));
  CHECK(world.Get<scene::Name>(e)->value == "Hero");
  // One undo reverses the whole group.
  CHECK(stack.Undo(world));
  CHECK(!world.Has<scene::Name>(e));
  CHECK(stack.Redo(world));
  CHECK(world.Has<scene::Name>(e));
  CHECK(world.Get<scene::Name>(e)->value == "Hero");

  // Destroy, then undo (entity recreated with same guid), then a stale-handle
  // command must still resolve through the guid.
  stack.Push(world, MakeDestroyEntity(world, e));
  CHECK(!world.IsAlive(e));
  CHECK(stack.Undo(world));
  ecs::Entity recreated = FindByGuid(world, guid);
  CHECK(recreated);
  CHECK(world.Get<scene::Name>(recreated)->value == "Hero");
  CHECK_NEAR(world.Get<scene::Transform>(recreated)->scale, 5.0f, 1e-6f);

  // A prop command captured against the OLD handle still works after recreation.
  CHECK(stack.Redo(world));  // redo destroy
  CHECK(!world.IsAlive(recreated));
  CHECK(stack.Undo(world));  // undo destroy again
  ecs::Entity again = FindByGuid(world, guid);
  CHECK(again);

  // Reparent preserving world transform.
  ecs::Entity p = world.Create();
  world.Add(p, scene::Transform{{100, 0, 0}, {0, 0, 0, 1}, 1.0f});
  scene::Transform before = WorldTransform(world, again);
  stack.Push(world, MakeReparent(world, again, p));
  CHECK(world.Has<scene::Parent>(again));
  scene::Transform after = WorldTransform(world, again);
  CHECK_NEAR(before.position[0], after.position[0], 1e-3f);
  CHECK_NEAR(before.position[1], after.position[1], 1e-3f);
  CHECK_NEAR(before.position[2], after.position[2], 1e-3f);
  CHECK(stack.Undo(world));
  CHECK(!world.Has<scene::Parent>(again));

  // Regression: the create command must not retain the caller's out pointer.
  // Callers pass stack addresses, so only the initial Apply inside Push may
  // write through it — undoing/redoing the creation must leave it untouched.
  {
    ecs::World w2;
    UndoStack s2;
    ecs::Entity created;
    s2.Push(w2, MakeCreateEntity({{transform, {}}}, &created));
    const ecs::Entity first = created;
    CHECK(w2.IsAlive(first));
    CHECK(s2.Undo(w2));
    CHECK(created == first);  // Revert must not clear it.
    CHECK(s2.Redo(w2));
    CHECK(created == first);  // Re-Apply must not write the new handle.
  }
}

void TestHierarchy() {
  ecs::World world;
  ecs::Entity parent = world.Create();
  // Parent translated +X by 10, rotated 90deg about Y, uniform scale 2.
  Quat ry = QuatFromAxisAngle({0, 1, 0}, 3.14159265f * 0.5f);
  world.Add(parent, scene::Transform{{10, 0, 0}, {ry.x, ry.y, ry.z, ry.w}, 2.0f});

  ecs::Entity child = world.Create();
  world.Add(child, scene::Transform{{1, 0, 0}, {0, 0, 0, 1}, 3.0f});
  world.Add(child, scene::Parent{parent});

  // Child local +X of 1, under parent scale 2 and 90deg-Y rotation, offset by
  // parent translation: world pos = (10,0,0) + R_y90 * (2 * (1,0,0)) = (10,0,-2).
  scene::Transform world_t = WorldTransform(world, child);
  CHECK_NEAR(world_t.position[0], 10.0f, 1e-3f);
  CHECK_NEAR(world_t.position[1], 0.0f, 1e-3f);
  CHECK_NEAR(world_t.position[2], -2.0f, 1e-3f);
  CHECK_NEAR(world_t.scale, 6.0f, 1e-3f);  // 2 * 3

  // WorldMatrix agrees with WorldTransform on the translation.
  Mat4 m = WorldMatrix(world, child);
  CHECK_NEAR(m.m[12], 10.0f, 1e-3f);
  CHECK_NEAR(m.m[14], -2.0f, 1e-3f);
}

void TestSelection() {
  Selection sel;
  ecs::Entity a{1, 0}, b{2, 0}, c{3, 0};
  sel.Set(a);
  CHECK(sel.Contains(a) && sel.primary() == a && sel.size() == 1);
  sel.Add(b);
  CHECK(sel.size() == 2 && sel.primary() == b);
  sel.Toggle(c);
  CHECK(sel.Contains(c) && sel.primary() == c);
  sel.Toggle(c);
  CHECK(!sel.Contains(c));
  sel.Clear();
  CHECK(sel.empty() && !sel.primary());
}

}  // namespace

int main() {
  TestReflection();
  TestSceneRoundTrip();
  TestUndo();
  TestHierarchy();
  TestSelection();
  if (failures == 0) std::printf("edit_test: all passed\n");
  return failures == 0 ? 0 : 1;
}
