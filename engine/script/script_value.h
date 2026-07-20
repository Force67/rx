#ifndef RX_SCRIPT_SCRIPT_VALUE_H_
#define RX_SCRIPT_SCRIPT_VALUE_H_

#include <type_traits>
#include <vector>

#include "core/math.h"
#include "core/types.h"
#include "ecs/entity.h"
#include "script/script_string.h"

namespace rx::script {

// The closed set of types that cross the handler boundary. This is the ONE type
// vocabulary every runtime and the wire codec speak; it mirrors edit::PropType
// but stays independent so the script layer does not depend on the editor.
// Everything here is a scalar, an entity id, a small POD (Vec3), or ScriptString
// -- no engine object pointers, no runtime types -- which is what keeps a handler
// serializable, replayable and testable without a VM.
enum class ScriptType : u8 {
  kVoid,
  kBool,
  kInt,
  kFloat,
  kVec3,
  kEntity,
  kString,  // content: an owned/borrowed ScriptString
  kSymbol,  // an interned identifier: just a StrId, resolved via ScriptSymbols
};

// One value on the "script stack": a call argument or a return. A hand-rolled
// tagged union, not std::variant -- for an 8-way value the variant is larger and
// copies/destroys through a hidden index switch, and holding an owning string
// would make it non-trivially-copyable. Here the string case is a borrowed VIEW
// (the arena or interner owns the bytes; the value never does), so ScriptValue is
// a trivially-copyable POD: the whole argument stack is memcpy-able, with no
// per-element construct/destroy. `type` is the single discriminant; reading a
// union member is gated on it (standard tagged-union discipline).
class ScriptValue {
 public:
  static ScriptValue Null() { return {}; }
  static ScriptValue Bool(bool v) { ScriptValue x; x.type_ = ScriptType::kBool; x.u_.b = v; return x; }
  static ScriptValue Int(i64 v) { ScriptValue x; x.type_ = ScriptType::kInt; x.u_.i = v; return x; }
  static ScriptValue Float(f64 v) { ScriptValue x; x.type_ = ScriptType::kFloat; x.u_.f = v; return x; }
  static ScriptValue Vec(Vec3 v) { ScriptValue x; x.type_ = ScriptType::kVec3; x.u_.v3 = v; return x; }
  static ScriptValue EntityRef(ecs::Entity v) { ScriptValue x; x.type_ = ScriptType::kEntity; x.u_.e = v; return x; }
  // Strings on the stack are borrowed views into arena/interner storage, never
  // owned. The bytes must outlive the call (a literal, an interned symbol, or an
  // ArenaCopy into the scratch heap) -- see ScriptArena / ScriptSymbols.
  static ScriptValue Str(ScriptStringView v) { ScriptValue x; x.type_ = ScriptType::kString; x.u_.s = v; return x; }
  static ScriptValue Symbol(StrId v) { ScriptValue x; x.type_ = ScriptType::kSymbol; x.u_.sym = v; return x; }

  ScriptType type() const { return type_; }
  bool is_null() const { return type_ == ScriptType::kVoid; }

  // Typed accessors: return the held value on a type match, else the default.
  bool as_bool(bool def = false) const { return type_ == ScriptType::kBool ? u_.b : def; }
  i64 as_int(i64 def = 0) const { return type_ == ScriptType::kInt ? u_.i : def; }
  f64 as_float(f64 def = 0.0) const { return type_ == ScriptType::kFloat ? u_.f : def; }
  Vec3 as_vec3(Vec3 def = {}) const { return type_ == ScriptType::kVec3 ? u_.v3 : def; }
  ecs::Entity as_entity(ecs::Entity def = {}) const { return type_ == ScriptType::kEntity ? u_.e : def; }
  ScriptStringView as_str() const { return type_ == ScriptType::kString ? u_.s : ScriptStringView{}; }
  StrId as_symbol(StrId def = StrId{0}) const { return type_ == ScriptType::kSymbol ? u_.sym : def; }

 private:
  // The NSDMI on the first member gives the union (and thus ScriptValue) a
  // defaulted default constructor even though Vec3/Entity have non-trivial
  // defaults; it does not affect trivial copyability.
  union Storage {
    i64 i = 0;
    bool b;
    f64 f;
    Vec3 v3;
    ecs::Entity e;
    StrId sym;
    ScriptStringView s;
  };

  ScriptType type_ = ScriptType::kVoid;
  Storage u_{};
};

// The point of the hand-rolled union: the value stack is a flat POD buffer.
static_assert(std::is_trivially_copyable_v<ScriptValue>,
              "ScriptValue must stay a POD so the argument stack is memcpy-able");

using ScriptStack = std::vector<ScriptValue>;

// A thin typed reader over the argument stack. This is the surface the unpacking
// trampolines use -- a.Ent(0), a.Vec(1), a.Str(0) -- so a handler's generated
// glue never indexes the raw stack or touches ScriptValue directly. Out-of-range
// or wrong-typed reads return a benign default, matching how a partially-wired
// runtime should degrade rather than crash.
class ScriptArgs {
 public:
  explicit ScriptArgs(const ScriptStack& stack) : stack_(stack) {}

  size_t size() const { return stack_.size(); }

  // Each accessor bounds-checks and returns the typed default on a miss. No
  // shared "null value" object: a function-local static would carry a
  // thread-safe-init guard (an atomic acquire-load on aarch64) on every argument
  // read, which is pure waste on the dispatch hot path.
  bool Bool(size_t i, bool def = false) const { auto* v = Ptr(i); return v ? v->as_bool(def) : def; }
  i32 I32(size_t i) const { auto* v = Ptr(i); return v ? static_cast<i32>(v->as_int()) : 0; }
  i64 I64(size_t i) const { auto* v = Ptr(i); return v ? v->as_int() : 0; }
  f32 F32(size_t i) const { auto* v = Ptr(i); return v ? static_cast<f32>(v->as_float()) : 0.0f; }
  f64 F64(size_t i) const { auto* v = Ptr(i); return v ? v->as_float() : 0.0; }
  Vec3 Vec(size_t i) const { auto* v = Ptr(i); return v ? v->as_vec3() : Vec3{}; }
  ecs::Entity Ent(size_t i) const { auto* v = Ptr(i); return v ? v->as_entity() : ecs::Entity{}; }
  ScriptStringView Str(size_t i) const { auto* v = Ptr(i); return v ? v->as_str() : ScriptStringView{}; }
  StrId Sym(size_t i) const { auto* v = Ptr(i); return v ? v->as_symbol() : StrId{0}; }

 private:
  const ScriptValue* Ptr(size_t i) const {
    return i < stack_.size() ? &stack_[i] : nullptr;
  }
  const ScriptStack& stack_;
};

}  // namespace rx::script

#endif  // RX_SCRIPT_SCRIPT_VALUE_H_
