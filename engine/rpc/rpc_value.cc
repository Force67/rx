#include "rpc/rpc_value.h"

namespace rx::rpc {

RpcValue::RpcValue() : v_(std::monostate{}) {}
RpcValue::RpcValue(bool v) : v_(v) {}
RpcValue::RpcValue(i64 v) : v_(v) {}
RpcValue::RpcValue(f64 v) : v_(v) {}
RpcValue::RpcValue(std::string v) : v_(std::move(v)) {}
RpcValue::RpcValue(std::vector<u8> v) : v_(std::move(v)) {}

RpcValue::Type RpcValue::type() const {
  return static_cast<Type>(v_.index());
}

bool RpcValue::is_null() const {
  return std::holds_alternative<std::monostate>(v_);
}

bool RpcValue::as_bool(bool def) const {
  if (const bool* p = std::get_if<bool>(&v_)) return *p;
  return def;
}

i64 RpcValue::as_int(i64 def) const {
  if (const i64* p = std::get_if<i64>(&v_)) return *p;
  return def;
}

f64 RpcValue::as_float(f64 def) const {
  if (const f64* p = std::get_if<f64>(&v_)) return *p;
  return def;
}

const std::string& RpcValue::as_string() const {
  static const std::string kEmpty;
  if (const std::string* p = std::get_if<std::string>(&v_)) return *p;
  return kEmpty;
}

const std::vector<u8>& RpcValue::as_blob() const {
  static const std::vector<u8> kEmpty;
  if (const std::vector<u8>* p = std::get_if<std::vector<u8>>(&v_)) return *p;
  return kEmpty;
}

bool RpcValue::operator==(const RpcValue& other) const {
  return v_ == other.v_;
}

}  // namespace rx::rpc
