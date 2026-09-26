#include "base/algorithm.h"
#include "base/check.h"
#include "base/memory/mem_ops.h"
#include "base/numeric_limits.h"
#include "base/strings/string_ref.h"
#include "script/script_string.h"

#include <string.h>

namespace rx::script {

ScriptString::ScriptString(ScriptStringView s) {
  if (s.size == 0) return;
  BASE_FATAL_CHECK(s.size != base::MinMax<u32>::max(), "script string longer than u32");
  data_ = new char[static_cast<size_t>(s.size) + 1];
  base::MemCopy(data_, s.data, s.size);
  data_[s.size] = '\0';
  size_ = s.size;
}

ScriptString::ScriptString(const char* s)
    : ScriptString(ScriptStringView(base::StringRef(s ? s : ""))) {}

ScriptString::ScriptString(const ScriptString& o) : ScriptString(o.view()) {}

ScriptString::ScriptString(ScriptString&& o) noexcept
    : data_(o.data_), size_(o.size_) {
  o.data_ = nullptr;
  o.size_ = 0;
}

ScriptString::~ScriptString() { delete[] data_; }

void ScriptString::swap(ScriptString& o) noexcept {
  base::Swap(data_, o.data_);
  base::Swap(size_, o.size_);
}

bool ScriptString::operator==(ScriptStringView o) const {
  return size_ == o.size && base::MemCompare(c_str(), o.data ? o.data : "", size_) == 0;
}

}  // namespace rx::script
