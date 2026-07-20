// rx::script acceptance: the portable string/value primitives, and driving the
// scene script handlers end-to-end against a REAL ecs::World with NO script
// runtime present -- no VM, no net, no game. The handlers call ecs::World and the
// scene components directly (no gateway); the test exercises them purely from a
// ScriptValue stack. Pure CPU logic; plain ctest runs it.

#include <cmath>
#include <cstdio>
#include <string>

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

  script::ScriptString empty;
  CHECK(empty.empty());
  CHECK(std::string(empty.c_str()).empty());
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
}

void TestSymbolsAndArena() {
  Rig rig;
  using V = script::ScriptValue;

  // World.Spawn(symbol "gold_ingot", pos, scale) -- a symbol arg (never a string
  // compare) that the handler stamps onto a real scene::Guid.
  const script::StrId prefab = rig.symbols.Intern("gold_ingot");
  V ret = rig.Call("World.Spawn", {V::Symbol(prefab), V::Vec(Vec3{5, 6, 7}), V::Float(2.0)});
  CHECK(ret.type() == script::ScriptType::kEntity);
  ecs::Entity spawned = ret.as_entity();
  CHECK(rig.world.IsAlive(spawned));
  CHECK(rig.world.Get<scene::Transform>(spawned)->position[0] == 5);

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
  TestSymbolsAndArena();
  if (g_failures == 0) {
    std::printf("script_test: all checks passed\n");
    return 0;
  }
  std::printf("script_test: %d failure(s)\n", g_failures);
  return 1;
}
