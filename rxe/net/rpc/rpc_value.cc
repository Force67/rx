#include "base/containers/vector.h"
#include "base/memory/move.h"
#include "base/strings/xstring.h"
#include "rxe/net/rpc/rpc_value.h"

namespace rx::rpc {

RpcValue::RpcValue() = default;
RpcValue::RpcValue(bool v) : type_(Type::kBool), bool_(v) {}
RpcValue::RpcValue(i64 v) : type_(Type::kInt), int_(v) {}
RpcValue::RpcValue(f64 v) : type_(Type::kFloat), float_(v) {}
RpcValue::RpcValue(base::String v) : type_(Type::kString), string_(base::move(v)) {}
RpcValue::RpcValue(base::Vector<u8> v) : type_(Type::kBlob), blob_(base::move(v)) {}

RpcValue::Type RpcValue::type() const {
  return type_;
}

bool RpcValue::is_null() const {
  return type_ == Type::kNull;
}

bool RpcValue::as_bool(bool def) const {
  return type_ == Type::kBool ? bool_ : def;
}

i64 RpcValue::as_int(i64 def) const {
  return type_ == Type::kInt ? int_ : def;
}

f64 RpcValue::as_float(f64 def) const {
  return type_ == Type::kFloat ? float_ : def;
}

const base::String& RpcValue::as_string() const {
  static const base::String kEmpty;
  return type_ == Type::kString ? string_ : kEmpty;
}

const base::Vector<u8>& RpcValue::as_blob() const {
  static const base::Vector<u8> kEmpty;
  return type_ == Type::kBlob ? blob_ : kEmpty;
}

// std::variant's equality: same alternative, then that alternative's ==. The
// float compares as a double does, so NaN is unequal to itself.
bool RpcValue::operator==(const RpcValue& other) const {
  if (type_ != other.type_) return false;
  switch (type_) {
    case Type::kNull: return true;
    case Type::kBool: return bool_ == other.bool_;
    case Type::kInt: return int_ == other.int_;
    case Type::kFloat: return float_ == other.float_;
    case Type::kString: return string_ == other.string_;
    case Type::kBlob: return blob_ == other.blob_;
  }
  return false;
}

}  // namespace rx::rpc
