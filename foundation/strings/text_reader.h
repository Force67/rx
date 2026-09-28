#ifndef RX_FOUNDATION_STRINGS_TEXT_READER_H_
#define RX_FOUNDATION_STRINGS_TEXT_READER_H_

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"

namespace rx {

// strtof / strtod in the "C" locale whatever setlocale says, so a host that
// localizes the process (a comma decimal point) cannot change how engine text
// parses. The std code this replaced was immune the same way: from_chars
// ignores locales, and istream parsed through the C++ locale, which setlocale
// does not touch.
RX_FOUNDATION_EXPORT f32 StrToF32(const char* text, char** end);
RX_FOUNDATION_EXPORT f64 StrToF64(const char* text, char** end);

// std::getline over text already in memory: yields the pieces between
// delimiters, keeps a '\r' before '\n', and yields no empty piece after a
// final delimiter. The text must outlive the reader.
class LineReader {
 public:
  explicit LineReader(base::StringRef text, char delimiter = '\n')
      : text_(text), delimiter_(delimiter) {}

  bool Next(base::StringRef* line) {
    if (pos_ >= text_.size()) return false;
    mem_size end = pos_;
    while (end < text_.size() && text_.data()[end] != delimiter_) ++end;
    *line = base::StringRef(text_.data() + pos_, end - pos_);
    pos_ = end + 1;
    return true;
  }

  // Offset of the first byte not yet returned, for a format that switches
  // from text lines to binary (a PLY header, say).
  mem_size position() const { return pos_ < text_.size() ? pos_ : text_.size(); }

 private:
  base::StringRef text_;
  char delimiter_;
  mem_size pos_ = 0;
};

// operator>> on an istringstream: whitespace-separated tokens, numbers parsed
// with the strto* functions libstdc++'s num_get ends in. A failed extraction
// zeroes its output and fails every later one, like failbit. The text must
// outlive the reader.
class TokenReader {
 public:
  explicit TokenReader(base::StringRef text) : text_(text) {}

  bool Next(base::StringRef* token) {
    if (failed_) return false;
    while (pos_ < text_.size() && IsSpace(text_.data()[pos_])) ++pos_;
    if (pos_ == text_.size()) return Fail();
    const mem_size start = pos_;
    while (pos_ < text_.size() && !IsSpace(text_.data()[pos_])) ++pos_;
    *token = base::StringRef(text_.data() + start, pos_ - start);
    return true;
  }

  bool Next(base::String* token) {
    base::StringRef piece;
    if (!Next(&piece)) return false;
    *token = base::String(piece.data(), piece.size());
    return true;
  }

  bool Next(f32* value) {
    return Parse(value, [](const char* s, char** end) { return StrToF32(s, end); },
                 /*allow_underflow=*/true);
  }
  bool Next(f64* value) {
    return Parse(value, [](const char* s, char** end) { return StrToF64(s, end); },
                 /*allow_underflow=*/true);
  }
  bool Next(i32* value) { return ParseInt(value, -2147483647 - 1, 2147483647); }
  bool Next(u32* value) { return ParseUnsigned(value, 0xffffffffull); }
  bool Next(i64* value) {
    return Parse(value, [](const char* s, char** end) { return ::strtoll(s, end, 10); });
  }
  bool Next(u64* value) { return ParseUnsigned(value, ~0ull); }

  // Whatever is left after the last extraction, leading whitespace included,
  // as getline(stream, rest) would return it.
  base::StringRef Rest() const {
    return base::StringRef(text_.data() + pos_, text_.size() - pos_);
  }

  bool ok() const { return !failed_; }
  explicit operator bool() const { return !failed_; }

 private:
  static bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
  }

  bool Fail() {
    failed_ = true;
    return false;
  }

  // num_get accepts a float that underflows (a denormal, or zero) and fails
  // one that overflows; for integers any ERANGE fails.
  template <typename T, typename F>
  bool Parse(T* value, F parse, bool allow_underflow = false) {
    base::StringRef token;
    if (!Next(&token)) {
      *value = 0;
      return false;
    }
    const base::String z(token.data(), token.size());
    char* end = nullptr;
    errno = 0;
    const auto parsed = parse(z.c_str(), &end);
    bool overflow = errno == ERANGE;
    if (overflow && allow_underflow) overflow = ::fabs(static_cast<f64>(parsed)) > 1.0;
    if (end == z.c_str() || overflow) {
      *value = 0;
      return Fail();
    }
    // A partial parse ("1.5x") gives back the unparsed tail for the next
    // extraction, as the stream would leave it unread.
    pos_ -= token.size() - static_cast<mem_size>(end - z.c_str());
    *value = static_cast<T>(parsed);
    return true;
  }

  template <typename T>
  bool ParseInt(T* value, i64 lo, i64 hi) {
    i64 wide = 0;
    if (!Parse(&wide, [](const char* s, char** end) { return ::strtoll(s, end, 10); })) {
      *value = 0;
      return false;
    }
    if (wide < lo || wide > hi) {
      *value = 0;
      return Fail();
    }
    *value = static_cast<T>(wide);
    return true;
  }

  template <typename T>
  bool ParseUnsigned(T* value, u64 hi) {
    u64 wide = 0;
    if (!Parse(&wide, [](const char* s, char** end) { return ::strtoull(s, end, 10); })) {
      *value = 0;
      return false;
    }
    if (wide > hi) {
      *value = 0;
      return Fail();
    }
    *value = static_cast<T>(wide);
    return true;
  }

  base::StringRef text_;
  mem_size pos_ = 0;
  bool failed_ = false;
};

// std::from_chars over the whole of |text|: nothing before or after the
// number, no '+', no hex. The float goes through strtof, not a double, so it
// is rounded once, to float, as from_chars rounds it.
inline bool ParseWholeF32(base::StringRef text, f32* out) {
  if (text.empty()) return false;
  const char c = text[0];
  if (!(c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 'i' || c == 'I' || c == 'n' ||
        c == 'N')) {
    return false;
  }
  for (char ch : text) {
    if (ch == 'x' || ch == 'X') return false;
  }
  const base::String z(text.data(), text.size());
  char* end = nullptr;
  errno = 0;
  const f32 parsed = StrToF32(z.c_str(), &end);
  if (end != z.c_str() + z.size() || errno == ERANGE) return false;
  *out = parsed;
  return true;
}

inline bool ParseWholeU32(base::StringRef text, u32* out) {
  if (text.empty()) return false;
  u64 value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') return false;
    value = value * 10 + static_cast<u64>(ch - '0');
    if (value > 0xffffffffull) return false;
  }
  *out = static_cast<u32>(value);
  return true;
}

}  // namespace rx

#endif  // RX_FOUNDATION_STRINGS_TEXT_READER_H_
