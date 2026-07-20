// Scene/spatial script handlers. Lives in the scene module (not the script
// substrate) so it can call ecs::World and the scene components DIRECTLY -- this
// is what replaces the old virtual gateway: the dependency points the right way
// (scene -> script), so no interface indirection is needed. Same three-part
// shape as every category: typed handlers, unpacking trampolines, Setup*.
#include "scene/scene_handlers.h"

#include "core/math.h"
#include "ecs/entity.h"
#include "ecs/world.h"
#include "scene/components.h"
#include "script/handler_context.h"
#include "script/handler_registry.h"
#include "script/script_arena.h"
#include "script/script_value.h"

namespace rx::scene {
namespace {

using namespace rx::script;  // ScriptValue, ScriptArgs, HandlerContext, ...

// Provenance of a script-spawned entity: which prefab symbol created it. A
// dedicated component, NOT scene::Guid -- Guid carries a per-entity-UNIQUE
// identity the serializer and undo system rely on, so stamping the shared prefab
// hash into it would collide sibling spawns and misresolve them.
struct SpawnedFrom {
  u64 prefab = 0;
};

// --- small direct helpers over the real components (no gateway) -------------
Vec3 ReadPos(ecs::World& w, ecs::Entity e) {
  if (Transform* t = w.Get<Transform>(e)) return {t->position[0], t->position[1], t->position[2]};
  return {};
}
void WritePos(ecs::World& w, ecs::Entity e, Vec3 p) {
  if (Transform* t = w.Get<Transform>(e)) {
    t->position[0] = p.x;
    t->position[1] = p.y;
    t->position[2] = p.z;
  } else {
    Transform nt;
    nt.position[0] = p.x;
    nt.position[1] = p.y;
    nt.position[2] = p.z;
    w.Add(e, nt);
  }
}

// ============================================================================
// 1. Handlers -- typed free functions that call the engine DIRECTLY through the
//    context's concrete ecs::World. PODs / entity ids / StrId / ScriptStringView
//    only; still trivially testable, now against a real (cheap) ecs::World.
// ============================================================================

// --- spatial ---
void Teleport(HandlerContext& c, ecs::Entity e, Vec3 position) {
  WritePos(c.Ecs(), e, position);
}
void MoveBy(HandlerContext& c, ecs::Entity e, Vec3 delta) {
  WritePos(c.Ecs(), e, ReadPos(c.Ecs(), e) + delta);
}
Vec3 GetPosition(HandlerContext& c, ecs::Entity e) { return ReadPos(c.Ecs(), e); }
f32 DistanceBetween(HandlerContext& c, ecs::Entity a, ecs::Entity b) {
  return Length(ReadPos(c.Ecs(), b) - ReadPos(c.Ecs(), a));
}
void SetScale(HandlerContext& c, ecs::Entity e, f32 scale) {
  if (Transform* t = c.Ecs().Get<Transform>(e)) t->scale = scale;
}
f32 GetScale(HandlerContext& c, ecs::Entity e) {
  Transform* t = c.Ecs().Get<Transform>(e);
  return t ? t->scale : 1.0f;
}

// --- lifecycle ---
bool IsValid(HandlerContext& c, ecs::Entity e) { return c.Ecs().IsAlive(e); }
ecs::Entity Spawn(HandlerContext& c, StrId prefab, Vec3 pos, f32 scale) {
  ecs::Entity e = c.Ecs().Create();
  Transform t;
  t.position[0] = pos.x;
  t.position[1] = pos.y;
  t.position[2] = pos.z;
  t.scale = scale;
  c.Ecs().Add(e, t);
  c.Ecs().Add(e, SpawnedFrom{static_cast<u64>(prefab)});  // remember the prefab
  return e;
}
void Destroy(HandlerContext& c, ecs::Entity e) { c.Ecs().Destroy(e); }

// --- identity: symbol in (scan), string in/out (arena) ---
ecs::Entity FindByPrefab(HandlerContext& c, StrId prefab) {
  const u64 want = static_cast<u64>(prefab);
  ecs::Entity found{};
  c.Ecs().Each<SpawnedFrom>([&](ecs::Entity e, SpawnedFrom& s) {
    if (s.prefab == want) found = e;
  });
  return found;
}
void SetName(HandlerContext& c, ecs::Entity e, ScriptStringView name) {
  std::string value(name.view());
  if (Name* n = c.Ecs().Get<Name>(e))
    n->value = std::move(value);
  else
    c.Ecs().Add(e, Name{std::move(value)});
}
// Returns a view into the per-call scratch heap (the arena owns the bytes); the
// ScriptValue on the stack only views them, so nothing is allocated on or copied
// through the global heap.
ScriptStringView GetName(HandlerContext& c, ecs::Entity e) {
  Name* n = c.Ecs().Get<Name>(e);
  return ArenaCopy(c.Heap(), n ? ScriptStringView(n->value) : ScriptStringView{});
}
void Log(HandlerContext& c, ScriptStringView message) { c.Log(message); }

// ============================================================================
// 2. Unpacking trampolines -- mechanical glue a generator/LLM emits.
// ============================================================================
ScriptValue Call_Teleport(HandlerContext& c, ScriptArgs& a) {
  Teleport(c, a.Ent(0), a.Vec(1));
  return ScriptValue::Null();
}
ScriptValue Call_MoveBy(HandlerContext& c, ScriptArgs& a) {
  MoveBy(c, a.Ent(0), a.Vec(1));
  return ScriptValue::Null();
}
ScriptValue Call_GetPosition(HandlerContext& c, ScriptArgs& a) {
  return ScriptValue::Vec(GetPosition(c, a.Ent(0)));
}
ScriptValue Call_DistanceBetween(HandlerContext& c, ScriptArgs& a) {
  return ScriptValue::Float(DistanceBetween(c, a.Ent(0), a.Ent(1)));
}
ScriptValue Call_SetScale(HandlerContext& c, ScriptArgs& a) {
  SetScale(c, a.Ent(0), a.F32(1));
  return ScriptValue::Null();
}
ScriptValue Call_GetScale(HandlerContext& c, ScriptArgs& a) {
  return ScriptValue::Float(GetScale(c, a.Ent(0)));
}
ScriptValue Call_IsValid(HandlerContext& c, ScriptArgs& a) {
  return ScriptValue::Bool(IsValid(c, a.Ent(0)));
}
ScriptValue Call_Spawn(HandlerContext& c, ScriptArgs& a) {
  return ScriptValue::EntityRef(Spawn(c, a.Sym(0), a.Vec(1), a.F32(2)));
}
ScriptValue Call_Destroy(HandlerContext& c, ScriptArgs& a) {
  Destroy(c, a.Ent(0));
  return ScriptValue::Null();
}
ScriptValue Call_FindByPrefab(HandlerContext& c, ScriptArgs& a) {
  return ScriptValue::EntityRef(FindByPrefab(c, a.Sym(0)));
}
ScriptValue Call_SetName(HandlerContext& c, ScriptArgs& a) {
  SetName(c, a.Ent(0), a.Str(1));
  return ScriptValue::Null();
}
ScriptValue Call_GetName(HandlerContext& c, ScriptArgs& a) {
  return ScriptValue::Str(GetName(c, a.Ent(0)));
}
ScriptValue Call_Log(HandlerContext& c, ScriptArgs& a) {
  Log(c, a.Str(0));
  return ScriptValue::Null();
}

}  // namespace

// ============================================================================
// 3. SetupSceneCommands -- the one exported symbol, called at engine start.
// ============================================================================
void SetupSceneCommands(HandlerRegistry& reg) {
  using T = ScriptType;
  reg.Add("World.Teleport", &Call_Teleport, {T::kVoid, {T::kEntity, T::kVec3}});
  reg.Add("World.MoveBy", &Call_MoveBy, {T::kVoid, {T::kEntity, T::kVec3}});
  reg.Add("World.GetPosition", &Call_GetPosition, {T::kVec3, {T::kEntity}});
  reg.Add("World.DistanceBetween", &Call_DistanceBetween,
          {T::kFloat, {T::kEntity, T::kEntity}});
  reg.Add("World.SetScale", &Call_SetScale, {T::kVoid, {T::kEntity, T::kFloat}});
  reg.Add("World.GetScale", &Call_GetScale, {T::kFloat, {T::kEntity}});
  reg.Add("World.IsValid", &Call_IsValid, {T::kBool, {T::kEntity}});
  reg.Add("World.Spawn", &Call_Spawn,
          {T::kEntity, {T::kSymbol, T::kVec3, T::kFloat}});
  reg.Add("World.Destroy", &Call_Destroy, {T::kVoid, {T::kEntity}});
  reg.Add("World.FindByPrefab", &Call_FindByPrefab, {T::kEntity, {T::kSymbol}});
  reg.Add("World.SetName", &Call_SetName, {T::kVoid, {T::kEntity, T::kString}});
  reg.Add("World.GetName", &Call_GetName, {T::kString, {T::kEntity}});
  reg.Add("World.Log", &Call_Log, {T::kVoid, {T::kString}});
}

}  // namespace rx::scene
