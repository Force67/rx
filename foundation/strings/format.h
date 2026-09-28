#ifndef RX_FOUNDATION_STRINGS_FORMAT_H_
#define RX_FOUNDATION_STRINGS_FORMAT_H_

#include "base/meta/traits.h"
#include "base/strings/format.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"

namespace rx {

// std::format for the engine, over base::FormatTo. The two differ where it
// shows: base writes a plain {} float as printf's %g (six digits) and reads
// {:.3} on a float as %.3f, where std::format writes the shortest text that
// reads back to the same value and reads {:.3} as %.3g. Scene files, the
// --validate report and the log were all written with std::format, so this
// keeps its answers: those two cases are done here, everything else is
// handed to base one placeholder at a time.
//
// The format string is checked at compile time, as std::format_string was:
// braces must balance, and every placeholder must name an argument.

// Appends the shortest text that parses back to exactly |value|, as
// std::to_chars and std::format("{}") produce it: fixed or scientific,
// whichever is shorter, fixed on a tie.
RX_FOUNDATION_EXPORT void AppendShortest(base::String& out, f32 value);
RX_FOUNDATION_EXPORT void AppendShortest(base::String& out, f64 value);

namespace format_detail {

// Compile-time: calling this from a consteval context fails the build with
// the message in the call site.
void FormatStringError(const char*);

consteval void Check(const char* s, mem_size arg_count) {
  mem_size next_auto = 0;
  bool used_manual = false;
  for (mem_size i = 0; s[i]; ++i) {
    if (s[i] == '}') {
      if (s[i + 1] != '}') FormatStringError("unmatched '}' in format string");
      ++i;
      continue;
    }
    if (s[i] != '{') continue;
    if (s[i + 1] == '{') {
      ++i;
      continue;
    }
    ++i;
    mem_size index = 0;
    bool manual = false;
    while (s[i] >= '0' && s[i] <= '9') {
      index = index * 10 + static_cast<mem_size>(s[i] - '0');
      manual = true;
      ++i;
    }
    if (manual) {
      used_manual = true;
    } else {
      if (used_manual) FormatStringError("mixed automatic and manual argument indices");
      index = next_auto++;
    }
    if (index >= arg_count) FormatStringError("format string names a missing argument");
    while (s[i] && s[i] != '}') ++i;
    if (!s[i]) FormatStringError("unterminated '{' in format string");
  }
}

// Keeps the format string from taking part in argument deduction.
template <typename T>
struct Identity {
  using type = T;
};

using Appender = void (*)(base::String& out, base::StringRef spec, const void* value);

template <typename T>
void Append(base::String& out, base::StringRef spec, const void* value) {
  const T& v = *static_cast<const T*>(value);
  if constexpr (base::is_floating_point_v<T>) {
    if (spec.empty()) {
      AppendShortest(out, v);
      return;
    }
  }
  // "{:" + spec + "}" for a single argument. A spec is a handful of chars.
  char buffer[64];
  mem_size n = 0;
  buffer[n++] = '{';
  buffer[n++] = ':';
  for (mem_size i = 0; i < spec.size() && n < sizeof(buffer) - 3; ++i) buffer[n++] = spec[i];
  if constexpr (base::is_floating_point_v<T>) {
    // std reads a precision with no type as general, base as fixed.
    const char last = spec[spec.size() - 1];
    const bool has_type = (last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z');
    if (!has_type && spec.find('.') != base::StringRef::npos) buffer[n++] = 'g';
  }
  buffer[n++] = '}';
  buffer[n] = '\0';
  base::FormatTo(out, buffer, v);
}

RX_FOUNDATION_EXPORT void FormatImpl(base::String& out, const char* fmt, const Appender* appenders,
                               const void* const* values, mem_size count);

}  // namespace format_detail

template <typename... Args>
struct FormatString {
  template <mem_size N>
  consteval FormatString(const char (&s)[N]) : str(s) {  // NOLINT(runtime/explicit)
    format_detail::Check(s, sizeof...(Args));
  }
  const char* str;
};

template <typename... Args>
void StrFormatTo(base::String& out, FormatString<typename format_detail::Identity<Args>::type...> fmt,
              const Args&... args) {
  if constexpr (sizeof...(Args) == 0) {
    format_detail::FormatImpl(out, fmt.str, nullptr, nullptr, 0);
  } else {
    const format_detail::Appender appenders[] = {&format_detail::Append<Args>...};
    const void* const values[] = {static_cast<const void*>(&args)...};
    format_detail::FormatImpl(out, fmt.str, appenders, values, sizeof...(Args));
  }
}

template <typename... Args>
base::String StrFormat(FormatString<typename format_detail::Identity<Args>::type...> fmt, const Args&... args) {
  base::String out;
  StrFormatTo<Args...>(out, fmt, args...);
  return out;
}

// std::to_string: integers in decimal, floating point as printf's %f, which
// base::ToString does not do (it keeps one or two decimals).
template <typename T>
base::String ToString(T value) {
  if constexpr (base::is_floating_point_v<T>) {
    return StrFormat("{:f}", value);
  } else {
    return StrFormat("{}", value);
  }
}

}  // namespace rx

#endif  // RX_FOUNDATION_STRINGS_FORMAT_H_
