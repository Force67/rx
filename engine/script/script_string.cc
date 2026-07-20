#include "script/script_string.h"

#include <cstring>
#include <utility>

namespace rx::script {

ScriptString::ScriptString(ScriptStringView s) {
  if (s.size == 0) return;
  data_ = new char[s.size + 1];
  std::memcpy(data_, s.data, s.size);
  data_[s.size] = '\0';
  size_ = s.size;
}

ScriptString::ScriptString(const char* s)
    : ScriptString(ScriptStringView(std::string_view(s ? s : ""))) {}

ScriptString::ScriptString(const ScriptString& o) : ScriptString(o.view()) {}

ScriptString::ScriptString(ScriptString&& o) noexcept
    : data_(o.data_), size_(o.size_) {
  o.data_ = nullptr;
  o.size_ = 0;
}

ScriptString::~ScriptString() { delete[] data_; }

void ScriptString::swap(ScriptString& o) noexcept {
  std::swap(data_, o.data_);
  std::swap(size_, o.size_);
}

bool ScriptString::operator==(ScriptStringView o) const {
  return size_ == o.size && std::memcmp(c_str(), o.data ? o.data : "", size_) == 0;
}

}  // namespace rx::script
