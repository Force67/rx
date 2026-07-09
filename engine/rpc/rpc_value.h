#ifndef RX_RPC_RPC_VALUE_H_
#define RX_RPC_RPC_VALUE_H_

#include <string>
#include <variant>
#include <vector>

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
  explicit RpcValue(std::string v);
  explicit RpcValue(std::vector<u8> v);  // kBlob

  Type type() const;
  bool is_null() const;

  // Typed accessors return the held value when the type matches, otherwise the
  // provided default. as_string and as_blob cannot return a default by value
  // cheaply, so on a type mismatch they return a reference to a shared empty
  // instance; the reference stays valid for the program lifetime.
  bool as_bool(bool def = false) const;
  i64 as_int(i64 def = 0) const;
  f64 as_float(f64 def = 0.0) const;
  const std::string& as_string() const;
  const std::vector<u8>& as_blob() const;

  bool operator==(const RpcValue& other) const;

 private:
  std::variant<std::monostate, bool, i64, f64, std::string, std::vector<u8>> v_;
};

using RpcArgs = std::vector<RpcValue>;

}  // namespace rx::rpc

#endif  // RX_RPC_RPC_VALUE_H_
