#include "authoring/command_bridge.h"

#include <format>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "ecs/entity.h"
#include "script/handler_context.h"
#include "script/handler_registry.h"
#include "script/script_string.h"
#include "script/script_symbols.h"
#include "script/script_value.h"

namespace rx::authoring {
namespace {

using script::ScriptType;
using script::ScriptValue;

// Wire values one signature param consumes. vec3 is the only param that is not
// one-to-one: rpc has no vector value, so it travels as three numbers.
size_t WireSlots(ScriptType type) { return type == ScriptType::kVec3 ? 3 : 1; }

bool IsNumber(const rpc::RpcValue& value) {
  return value.type() == rpc::RpcValue::Type::kInt ||
         value.type() == rpc::RpcValue::Type::kFloat;
}

// An int where a float is wanted is a widening, not a reinterpretation, and it
// is the difference between `SetScale e 2` working and needing `2.0`. The
// reverse is not accepted: truncating a caller's 2.7 into an entity index or a
// count would be guessing at intent.
f64 AsNumber(const rpc::RpcValue& value) {
  return value.type() == rpc::RpcValue::Type::kInt ? static_cast<f64>(value.as_int())
                                                   : value.as_float();
}

const char* WireTypeName(rpc::RpcValue::Type type) {
  switch (type) {
    case rpc::RpcValue::Type::kNull: return "null";
    case rpc::RpcValue::Type::kBool: return "bool";
    case rpc::RpcValue::Type::kInt: return "int";
    case rpc::RpcValue::Type::kFloat: return "float";
    case rpc::RpcValue::Type::kString: return "string";
    case rpc::RpcValue::Type::kBlob: return "blob";
  }
  return "unknown";
}

// "(entity, vec3)" -- the signature as the caller has to spell it, for a
// mismatch message that says what was wanted rather than only what was wrong.
std::string SigText(const script::HandlerSig& sig) {
  std::string text = "(";
  for (u8 i = 0; i < sig.count; ++i) {
    if (i) text += ", ";
    text += script::ScriptTypeName(sig.params[i]);
  }
  return text + ")";
}

// Entity ids are two u32s; the wire has one integer, so they pack into it. Low
// half is the index, high half the generation, which keeps a freshly created
// entity a small readable number for whoever is typing calls by hand.
i64 PackEntity(ecs::Entity e) {
  return static_cast<i64>(static_cast<u64>(e.index) |
                          (static_cast<u64>(e.generation) << 32));
}

ecs::Entity UnpackEntity(i64 packed) {
  const u64 bits = static_cast<u64>(packed);
  return ecs::Entity{static_cast<u32>(bits), static_cast<u32>(bits >> 32)};
}

std::string TypeError(size_t index, ScriptType want, const rpc::RpcValue& got) {
  return std::format("arg {} expects {}, got {}", index, script::ScriptTypeName(want),
                     WireTypeName(got.type()));
}

// HandlerSig decides the arity and every conversion; a call that does not match
// is refused here rather than reaching a handler that would read the wrong union
// member out of a defaulted ScriptValue.
bool BuildStack(const script::HandlerSig& sig, const rpc::RpcArgs& args,
                script::HandlerContext& ctx, script::ScriptStack* stack,
                std::string* error) {
  size_t want = 0;
  for (u8 p = 0; p < sig.count; ++p) want += WireSlots(sig.params[p]);
  if (args.size() != want) {
    *error = std::format("expects {} arg(s) for {}, got {}", want, SigText(sig),
                         args.size());
    return false;
  }

  size_t at = 0;
  for (u8 p = 0; p < sig.count; ++p) {
    const rpc::RpcValue& value = args[at];
    switch (sig.params[p]) {
      case ScriptType::kBool:
        if (value.type() != rpc::RpcValue::Type::kBool) {
          *error = TypeError(at, ScriptType::kBool, value);
          return false;
        }
        stack->push_back(ScriptValue::Bool(value.as_bool()));
        break;
      case ScriptType::kInt:
        if (value.type() != rpc::RpcValue::Type::kInt) {
          *error = TypeError(at, ScriptType::kInt, value);
          return false;
        }
        stack->push_back(ScriptValue::Int(value.as_int()));
        break;
      case ScriptType::kFloat:
        if (!IsNumber(value)) {
          *error = TypeError(at, ScriptType::kFloat, value);
          return false;
        }
        stack->push_back(ScriptValue::Float(AsNumber(value)));
        break;
      case ScriptType::kEntity:
        if (value.type() != rpc::RpcValue::Type::kInt) {
          *error = TypeError(at, ScriptType::kEntity, value);
          return false;
        }
        stack->push_back(ScriptValue::EntityRef(UnpackEntity(value.as_int())));
        break;
      case ScriptType::kVec3: {
        f32 xyz[3];
        for (size_t c = 0; c < 3; ++c) {
          if (!IsNumber(args[at + c])) {
            *error = TypeError(at + c, ScriptType::kFloat, args[at + c]);
            return false;
          }
          xyz[c] = static_cast<f32>(AsNumber(args[at + c]));
        }
        stack->push_back(ScriptValue::Vec(Vec3{xyz[0], xyz[1], xyz[2]}));
        break;
      }
      case ScriptType::kString:
        if (value.type() != rpc::RpcValue::Type::kString) {
          *error = TypeError(at, ScriptType::kString, value);
          return false;
        }
        // Borrowed, not copied: the RpcCall owns the bytes and outlives the
        // synchronous dispatch (see CommandBridge::Invoke).
        stack->push_back(ScriptValue::Str(script::ScriptStringView(value.as_string())));
        break;
      case ScriptType::kSymbol:
        if (value.type() != rpc::RpcValue::Type::kString) {
          *error = TypeError(at, ScriptType::kSymbol, value);
          return false;
        }
        stack->push_back(
            ScriptValue::Symbol(ctx.Syms().Intern(script::ScriptStringView(value.as_string()))));
        break;
      case ScriptType::kVoid:
        *error = std::format("arg {} has no wire representation (void)", at);
        return false;
    }
    at += WireSlots(sig.params[p]);
  }
  return true;
}

// The return travels back as rpc values. A returned string is COPIED out of the
// context's scratch arena here: the arena is reset once the caller is done
// polling, and the reply outlives that.
bool BuildReply(ScriptType ret, const ScriptValue& value, script::HandlerContext& ctx,
                rpc::RpcArgs* out, std::string* error) {
  switch (ret) {
    case ScriptType::kVoid:
      return true;
    case ScriptType::kBool:
      out->emplace_back(value.as_bool());
      return true;
    case ScriptType::kInt:
      out->emplace_back(value.as_int());
      return true;
    case ScriptType::kFloat:
      out->emplace_back(value.as_float());
      return true;
    case ScriptType::kEntity:
      out->emplace_back(PackEntity(value.as_entity()));
      return true;
    case ScriptType::kVec3: {
      const Vec3 v = value.as_vec3();
      out->emplace_back(static_cast<f64>(v.x));
      out->emplace_back(static_cast<f64>(v.y));
      out->emplace_back(static_cast<f64>(v.z));
      return true;
    }
    case ScriptType::kString:
      out->emplace_back(std::string(value.as_str().view()));
      return true;
    case ScriptType::kSymbol:
      out->emplace_back(std::string(ctx.Syms().Resolve(value.as_symbol()).view()));
      return true;
  }
  *error = "command returns a type with no wire representation";
  return false;
}

}  // namespace

CommandBridge::CommandBridge(script::HandlerRegistry& commands, script::HandlerContext& ctx)
    : commands_(commands), ctx_(ctx) {
  for (size_t i = 0; i < commands.size(); ++i) {
    const script::HandlerDesc& desc = commands.at(i);
    // Both captures are safe by value: a command name views static storage (a
    // string literal, see HandlerRegistry::Add) and HandlerSig is a POD. Nothing
    // captures the HandlerDesc address, which a later Add would relocate.
    const script::ScriptStringView name = desc.name;
    const script::HandlerSig sig = desc.sig;
    registry_.On(std::string(name.view()),
                 [this, name, sig](const rpc::RpcContext&, const rpc::RpcArgs& args) {
                   // No reply slot means this Dispatch did not come through
                   // Invoke, so it never passed the trust check: do nothing.
                   if (!pending_) return;
                   Reply& reply = *pending_;
                   script::ScriptStack stack;
                   stack.reserve(sig.count);
                   std::string detail;
                   if (!BuildStack(sig, args, ctx_, &stack, &detail)) {
                     reply.error = std::format("{}: {}", name.view(), detail);
                     return;
                   }
                   script::ScriptArgs script_args(stack);
                   const ScriptValue result = commands_.Dispatch(ctx_, name, script_args);
                   reply.ok = BuildReply(sig.ret, result, ctx_, &reply.values, &detail);
                   if (!reply.ok) reply.error = std::format("{}: {}", name.view(), detail);
                 });
  }
}

CommandBridge::Reply CommandBridge::Invoke(const rpc::RpcContext& ctx,
                                           const rpc::RpcCall& call) {
  Reply reply;
  if (ctx.sender != kLocalSender || ctx.from_server) {
    // Deliberately terse to the caller (it learns nothing about what exists) and
    // loud in the log, where an operator can see who tried.
    RX_WARN("authoring: refused '{}' from sender {} (from_server={})", call.name,
            ctx.sender, ctx.from_server);
    reply.error = "refused: authoring commands are local-endpoint only";
    return reply;
  }
  pending_ = &reply;
  const bool dispatched = registry_.Dispatch(ctx, call);
  pending_ = nullptr;
  if (!dispatched) reply.error = std::format("unknown command '{}'", call.name);
  return reply;
}

}  // namespace rx::authoring
