#ifndef RX_SCRIPT_SCRIPT_STRING_H_
#define RX_SCRIPT_SCRIPT_STRING_H_

#include "base/check.h"
#include "base/numeric_limits.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::script {

// The string type at the script-handler boundary. Deliberately NOT base::String:
// handlers are bound to several script runtimes (the Papyrus VM, the managed
// host, the RPC/net path, a console), and base::String's layout is compiler- and
// stdlib-specific, so it cannot cross a runtime or DSO boundary safely. These
// two types give a stable, self-owned representation instead, decoupled from any
// one runtime and from any game (rx only, no Skyrim/recreation types leak in).

// The boundary/argument type: a non-owning view with a fixed, trivially-copyable
// 16-byte layout. This is what a handler that only READS a string takes, and
// what every runtime marshals its own string into. UTF-8; not guaranteed
// null-terminated (use view()).
struct ScriptStringView {
  const char* data = nullptr;
  u32 size = 0;

  constexpr ScriptStringView() = default;
  constexpr ScriptStringView(const char* d, u32 n) : data(d), size(n) {}
  constexpr ScriptStringView(const char* s)  // NOLINT(google-explicit-constructor)
      : data(s), size(0) {
    size_t length = 0;
    while (s && s[length] != '\0') {
      BASE_FATAL_CHECK(length < base::MinMax<u32>::max(), "script string longer than u32");
      ++length;
    }
    size = static_cast<u32>(length);
  }
  ScriptStringView(base::StringRef s)  // NOLINT(google-explicit-constructor)
      : data(s.data()) {
    BASE_FATAL_CHECK(s.size() <= base::MinMax<u32>::max(), "script string longer than u32");
    size = static_cast<u32>(s.size());
  }
  // A base::String reaches base::StringRef through two viable constructor
  // templates there; MSVC calls that ambiguous, so take it directly.
  ScriptStringView(const base::String& s)  // NOLINT(google-explicit-constructor)
      : ScriptStringView(base::StringRef(s.data(), s.size(), /*is_null_terminated=*/true)) {}

  base::StringRef view() const { return {data, size}; }
  operator base::StringRef() const { return view(); }  // NOLINT
  bool empty() const { return size == 0; }
};
static_assert(sizeof(ScriptStringView) == 16, "ABI-boundary layout must stay fixed");

// The owning value: what a handler that RETURNS or STORES a string yields. Owns
// a null-terminated UTF-8 buffer with an explicit layout (not base::String), and
// serializes as a u32 length followed by its bytes.
class ScriptString {
 public:
  ScriptString() = default;
  RX_SCRIPT_EXPORT ScriptString(ScriptStringView s);  // NOLINT
  ScriptString(base::StringRef s) : ScriptString(ScriptStringView(s)) {}  // NOLINT
  RX_SCRIPT_EXPORT ScriptString(const char* s);  // NOLINT
  RX_SCRIPT_EXPORT ScriptString(const ScriptString& o);
  RX_SCRIPT_EXPORT ScriptString(ScriptString&& o) noexcept;
  ScriptString& operator=(ScriptString o) noexcept {
    swap(o);
    return *this;
  }
  RX_SCRIPT_EXPORT ~ScriptString();

  RX_SCRIPT_EXPORT void swap(ScriptString& o) noexcept;

  ScriptStringView view() const { return {data_, size_}; }
  const char* c_str() const { return data_ ? data_ : ""; }
  u32 size() const { return size_; }
  bool empty() const { return size_ == 0; }

  RX_SCRIPT_EXPORT bool operator==(ScriptStringView o) const;
  bool operator==(const ScriptString& o) const { return *this == o.view(); }

 private:
  char* data_ = nullptr;  // owned, null-terminated (data_[size_] == '\0')
  u32 size_ = 0;
};

// Interned identifier hash for handler/category names. Names are compile-time
// tokens, not content, so dispatch keys on this 64-bit hash rather than dragging
// a string type onto the hot path. FNV-1a, so a runtime can hash a name once
// and dispatch by id.
enum class StrId : u64 {};

inline StrId HashStr(base::StringRef s) {
  return static_cast<StrId>(Fnv1a(s));
}

}  // namespace rx::script

#endif  // RX_SCRIPT_SCRIPT_STRING_H_
