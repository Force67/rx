// rx::script acceptance: the portable string/value primitives, and driving the
// scene script handlers end-to-end against a REAL ecs::World with NO script
// runtime present -- no VM, no net, no game. The handlers call ecs::World and the
// scene components directly (no gateway); the test exercises them purely from a
// ScriptValue stack. Pure CPU logic; plain ctest runs it.

#include <cmath>
#include <cstdio>
#include <limits>
#include <new>
#include <string>
#include <utility>

#include "core/math.h"
#include "ecs/entity.h"
#include "ecs/world.h"
#include "scene/components.h"
#include "scene/scene_handlers.h"
#include "script/handler_context.h"
#include "script/handler_registry.h"
#include "script/script_arena.h"
#include "script/script_string.h"
#include "script/script_symbols.h"
#include "script/script_value.h"

namespace {

int g_failures = 0;

#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

namespace script = rx::script;
namespace scene = rx::scene;
namespace ecs = rx::ecs;
using rx::f32;
using rx::Vec3;

std::string g_log;
void LogSink(void* user, script::ScriptStringView msg) {
  static_cast<std::string*>(user)->assign(msg.view());
}

void TestScriptString() {
  script::ScriptString s("hello");
  CHECK(s.size() == 5);
  CHECK(s == script::ScriptStringView("hello", 5));
  CHECK(std::string(s.c_str()) == "hello");

  script::ScriptString copy = s;  // copy ctor -> deep copy
  CHECK(copy == s);
  script::ScriptString moved = std::move(copy);  // move ctor
  CHECK(moved == s);
  CHECK(moved.view().view() == "hello");

  CHECK(script::HashStr("World.Teleport") == script::HashStr("World.Teleport"));
  CHECK(script::HashStr("World.Teleport") != script::HashStr("World.MoveBy"));
  CHECK(static_cast<rx::u64>(script::HashStr("World.Teleport")) ==
        rx::Fnv1a("World.Teleport"));

  script::ScriptString empty;
  CHECK(empty.empty());
  CHECK(std::string(empty.c_str()).empty());

  bool rejected = false;
  try {
    script::ScriptString too_large(
        script::ScriptStringView(nullptr, std::numeric_limits<rx::u32>::max()));
  } catch (const std::bad_array_new_length&) {
    rejected = true;
  }
  CHECK(rejected);

  script::ScriptArena arena(16);
  rejected = false;
  try {
    script::ArenaCopy(
        arena, script::ScriptStringView(nullptr, std::numeric_limits<rx::u32>::max()));
  } catch (const std::bad_array_new_length&) {
    rejected = true;
  }
  CHECK(rejected);

  rejected = false;
  try {
    arena.Alloc(std::numeric_limits<size_t>::max(), alignof(std::max_align_t));
  } catch (const std::bad_array_new_length&) {
    rejected = true;
  }
  CHECK(rejected);

  rejected = false;
  try {
    arena.Alloc(1, 3);
  } catch (const std::bad_array_new_length&) {
    rejected = true;
  }
  CHECK(rejected);
}

void TestInterner() {
  script::ScriptSymbols symbols;
  script::StrId a = symbols.Intern("DragonPriestMask");
  script::StrId b = symbols.Intern("DragonPriestMask");  // dedup
  CHECK(a == b);
  CHECK(symbols.size() == 1);
  CHECK(static_cast<rx::u64>(a) ==
        static_cast<rx::u64>(script::HashStr("DragonPriestMask")));
  CHECK(symbols.Resolve(a).view() == "DragonPriestMask");
  CHECK(symbols.Has(a));
  script::StrId c = symbols.Intern("Daedric");
  CHECK(c != a);
  CHECK(symbols.size() == 2);
  CHECK(!symbols.Has(script::HashStr("NeverInterned")));
}

// One rig: a real world, the interner, a scratch heap, and the registry with the
// scene commands installed. This is what the app wires at engine start.
struct Rig {
  ecs::World world;
  script::ScriptSymbols symbols;
  script::ScriptArena scratch;
  script::HandlerRegistry reg;
  script::HandlerContext ctx;

  Rig() {
    scene::SetupSceneCommands(reg);
    ctx.world = &world;
    ctx.symbols = &symbols;
    ctx.scratch = &scratch;
    ctx.log_sink = &LogSink;
    ctx.log_user = &g_log;
  }

  script::ScriptValue Call(script::ScriptStringView name, script::ScriptStack stack) {
    script::ScriptArgs args(stack);
    return reg.Dispatch(ctx, name, args);
  }
};

void TestDispatch() {
  Rig rig;
  CHECK(rig.reg.Has("World.Teleport"));
  CHECK(!rig.reg.Has("World.DoesNotExist"));
  CHECK(rig.reg.size() == 13);

  ecs::Entity e = rig.world.Create();

  using V = script::ScriptValue;
  // World.Teleport(e, {1,2,3}) -- args pushed as a ScriptValue stack, exactly the
  // shape a runtime would produce. The handler writes a real scene::Transform.
  V ret = rig.Call("World.Teleport", {V::EntityRef(e), V::Vec(Vec3{1, 2, 3})});
  CHECK(ret.is_null());
  scene::Transform* t = rig.world.Get<scene::Transform>(e);
  CHECK(t != nullptr);
  CHECK(t->position[0] == 1 && t->position[1] == 2 && t->position[2] == 3);

  rig.Call("World.MoveBy", {V::EntityRef(e), V::Vec(Vec3{10, 0, 0})});
  CHECK(rig.world.Get<scene::Transform>(e)->position[0] == 11);

  ret = rig.Call("World.GetPosition", {V::EntityRef(e)});
  CHECK(ret.type() == script::ScriptType::kVec3);
  CHECK(ret.as_vec3().x == 11);

  rig.Call("World.SetScale", {V::EntityRef(e), V::Float(3.0)});
  ret = rig.Call("World.GetScale", {V::EntityRef(e)});
  CHECK(std::fabs(ret.as_float() - 3.0) < 1e-5);

  ret = rig.Call("World.IsValid", {V::EntityRef(e)});
  CHECK(ret.as_bool() == true);

  // Unknown handler: dropped, not an error, returns Null.
  CHECK(rig.Call("World.DoesNotExist", {}).is_null());

  // Recorded signature is the machine-readable source of truth for codegen/wire.
  const script::HandlerDesc* desc = rig.reg.Find("World.Teleport");
  CHECK(desc && desc->sig.count == 2);
  CHECK(desc->sig.params[0] == script::ScriptType::kEntity);
  CHECK(desc->sig.params[1] == script::ScriptType::kVec3);
  CHECK(desc->name.view() == "World.Teleport");  // stored as a view, not copied

  // Mutating commands must reject stale and malformed entity handles. The ECS
  // reuses the destroyed slot, so writing through `stale` would corrupt `live`
  // if the generation were not checked before the Add fallback.
  ecs::Entity stale = rig.world.Create();
  rig.world.Destroy(stale);
  ecs::Entity live = rig.world.Create();
  CHECK(live.index == stale.index && live.generation != stale.generation);
  rig.world.Add(live, scene::Transform{{7, 0, 0}});
  rig.world.Add(live, scene::Name{"live"});
  rig.Call("World.Teleport", {V::EntityRef(stale), V::Vec(Vec3{99, 0, 0})});
  rig.Call("World.SetName", {V::EntityRef(stale), V::Str("stale")});
  rig.Call("World.Teleport", {});
  CHECK(rig.world.Get<scene::Transform>(live)->position[0] == 7);
  CHECK(rig.world.Get<scene::Name>(live)->value == "live");
}

void TestWorldSpaceDispatch() {
  Rig rig;
  using V = script::ScriptValue;

  ecs::Entity parent = rig.world.Create();
  const rx::Quat rz = rx::QuatFromAxisAngle({0, 0, 1}, 3.14159265f * 0.5f);
  rig.world.Add(parent,
                scene::Transform{{10, 0, 0}, {rz.x, rz.y, rz.z, rz.w}, 2.0f});
  ecs::Entity child = rig.world.Create();
  rig.world.Add(child, scene::Transform{{1, 0, 0}});
  rig.world.Add(child, scene::Parent{parent});

  V ret = rig.Call("World.GetPosition", {V::EntityRef(child)});
  CHECK(std::fabs(ret.as_vec3().x - 10.0f) < 1e-4f);
  CHECK(std::fabs(ret.as_vec3().y - 2.0f) < 1e-4f);

  rig.Call("World.Teleport", {V::EntityRef(child), V::Vec(Vec3{8, 0, 0})});
  scene::Transform* local = rig.world.Get<scene::Transform>(child);
  CHECK(std::fabs(local->position[0] - 0.0f) < 1e-4f);
  CHECK(std::fabs(local->position[1] - 1.0f) < 1e-4f);
  ret = rig.Call("World.GetPosition", {V::EntityRef(child)});
  CHECK(std::fabs(ret.as_vec3().x - 8.0f) < 1e-4f);
  CHECK(std::fabs(ret.as_vec3().y - 0.0f) < 1e-4f);

  ecs::Entity other = rig.world.Create();
  rig.world.Add(other, scene::Transform{{8, 0, 0}});
  ret = rig.Call("World.DistanceBetween", {V::EntityRef(child), V::EntityRef(other)});
  CHECK(std::fabs(ret.as_float()) < 1e-4);

  // Any nonzero parent scale is invertible, even below the editor's normal
  // range. Teleport must not silently reject it as singular.
  ecs::Entity tiny_parent = rig.world.Create();
  rig.world.Add(tiny_parent, scene::Transform{{0, 0, 0}, {0, 0, 0, 1}, 1e-14f});
  ecs::Entity tiny_child = rig.world.Create();
  rig.world.Add(tiny_child, scene::Transform{});
  rig.world.Add(tiny_child, scene::Parent{tiny_parent});
  rig.Call("World.Teleport", {V::EntityRef(tiny_child), V::Vec(Vec3{1, 0, 0})});
  ret = rig.Call("World.GetPosition", {V::EntityRef(tiny_child)});
  CHECK(std::fabs(ret.as_vec3().x - 1.0f) < 1e-4f);

  // Over-depth hierarchies are rejected consistently: reads report no position
  // and writes leave the local transform untouched.
  ecs::Entity ancestor = rig.world.Create();
  for (int i = 0; i < 4097; ++i) {
    ecs::Entity descendant = rig.world.Create();
    rig.world.Add(descendant, scene::Parent{ancestor});
    ancestor = descendant;
  }
  rig.world.Add(ancestor, scene::Transform{{3, 0, 0}});
  ret = rig.Call("World.GetPosition", {V::EntityRef(ancestor)});
  CHECK(ret.as_vec3().x == 0);
  rig.Call("World.Teleport", {V::EntityRef(ancestor), V::Vec(Vec3{5, 0, 0})});
  CHECK(rig.world.Get<scene::Transform>(ancestor)->position[0] == 3);
}

void TestSymbolsAndArena() {
  Rig rig;
  using V = script::ScriptValue;

  // World.Spawn(symbol "gold_ingot", pos, scale) -- a symbol arg (never a string
  // compare) that the handler records in persistent prefab provenance.
  const script::StrId prefab = rig.symbols.Intern("gold_ingot");
  V ret = rig.Call("World.Spawn", {V::Symbol(prefab), V::Vec(Vec3{5, 6, 7}), V::Float(2.0)});
  CHECK(ret.type() == script::ScriptType::kEntity);
  ecs::Entity spawned = ret.as_entity();
  CHECK(rig.world.IsAlive(spawned));
  CHECK(rig.world.Get<scene::Transform>(spawned)->position[0] == 5);
  CHECK(rig.world.Get<scene::SpawnedFrom>(spawned)->prefab ==
        static_cast<rx::u64>(prefab));

  // World.FindByPrefab(symbol) -- scans provenance components, finds the spawned one
  // (also verifies the prefab was recorded, without peeking at an internal type).
  ret = rig.Call("World.FindByPrefab", {V::Symbol(prefab)});
  CHECK(ret.as_entity().index == spawned.index);

  // World.SetName / GetName -- string IN (copied to a real scene::Name), string
  // OUT (arena-backed borrowed ScriptString, no global heap).
  // String args are borrowed views; the literal's bytes have static storage.
  rig.Call("World.SetName", {V::EntityRef(spawned), V::Str("Gold Ingot")});
  CHECK(rig.world.Get<scene::Name>(spawned)->value == "Gold Ingot");
  ret = rig.Call("World.GetName", {V::EntityRef(spawned)});
  CHECK(ret.type() == script::ScriptType::kString);
  CHECK(ret.as_str().view() == "Gold Ingot");

  // World.Log routes a ScriptStringView across the boundary to the sink.
  rig.Call("World.Log", {V::Str("hi from script")});
  CHECK(g_log == "hi from script");

  // The scratch heap took real allocations (the GetName copy); Reset reclaims all.
  CHECK(rig.scratch.bytes_used() > 0);
  rig.scratch.Reset();
  CHECK(rig.scratch.bytes_used() == 0);

  const script::HandlerDesc* spawn = rig.reg.Find("World.Spawn");
  CHECK(spawn && spawn->sig.params[0] == script::ScriptType::kSymbol);
}

}  // namespace

int main() {
  TestScriptString();
  TestInterner();
  TestDispatch();
  TestWorldSpaceDispatch();
  TestSymbolsAndArena();
  if (g_failures == 0) {
    std::printf("script_test: all checks passed\n");
    return 0;
  }
  std::printf("script_test: %d failure(s)\n", g_failures);
  return 1;
}
