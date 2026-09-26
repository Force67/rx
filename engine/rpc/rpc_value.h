#ifndef RX_RPC_RPC_VALUE_H_
#define RX_RPC_RPC_VALUE_H_

#include "base/containers/vector.h"
#include "base/strings/xstring.h"
#include "core/types.h"

namespace rx::rpc {

// A compact dynamic value for one RPC argument. RPC calls cross the wire between
// untrusted peers, so arguments cannot be a fixed struct; each handler decides
// what it expects and pulls typed values out with the as_* accessors. The value
// holds exactly one of a small closed set of types (no nested containers), which
// keeps the wire codec trivial and the decode path easy to bound.
class RpcValue {
 public:
  enum class Type : u8 { kNull, kBool, kInt, kFloat, kString, kBlob };

  RpcValue();  // kNull
  explicit RpcValue(bool v);
  explicit RpcValue(i64 v);
  explicit RpcValue(f64 v);
  explicit RpcValue(base::String v);
  explicit RpcValue(base::Vector<u8> v);  // kBlob

  Type type() const;
  bool is_null() const;

  // Typed accessors return the held value when the type matches, otherwise the
  // provided default. as_string and as_blob cannot return a default by value
  // cheaply, so on a type mismatch they return a reference to a shared empty
  // instance; the reference stays valid for the program lifetime.
  bool as_bool(bool def = false) const;
  i64 as_int(i64 def = 0) const;
  f64 as_float(f64 def = 0.0) const;
  const base::String& as_string() const;
  const base::Vector<u8>& as_blob() const;

  bool operator==(const RpcValue& other) const;

 private:
  // A tagged value rather than a union: the string and blob own heap memory,
  // and one live member at a time buys nothing at these sizes.
  Type type_ = Type::kNull;
  bool bool_ = false;
  i64 int_ = 0;
  f64 float_ = 0.0;
  base::String string_;
  base::Vector<u8> blob_;
};

using RpcArgs = base::Vector<RpcValue>;

}  // namespace rx::rpc

#endif  // RX_RPC_RPC_VALUE_H_
