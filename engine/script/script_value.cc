// ScriptValue and ScriptArgs are defined inline in the header (a small closed
// variant with no out-of-line body). This translation unit exists so the header
// is compiled standalone as part of the module and has a place for any future
// out-of-line members (e.g. a wire codec) to land.
#include "script/script_value.h"

namespace rx::script {

const char* ScriptTypeName(ScriptType type) {
  switch (type) {
    case ScriptType::kVoid: return "void";
    case ScriptType::kBool: return "bool";
    case ScriptType::kInt: return "int";
    case ScriptType::kFloat: return "float";
    case ScriptType::kVec3: return "vec3";
    case ScriptType::kEntity: return "entity";
    case ScriptType::kString: return "string";
    case ScriptType::kSymbol: return "symbol";
  }
  return "unknown";
}

}  // namespace rx::script
