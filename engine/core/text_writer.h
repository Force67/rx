#ifndef RX_CORE_TEXT_WRITER_H_
#define RX_CORE_TEXT_WRITER_H_

#include "base/memory/move.h"
#include "base/strings/format.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/types.h"

namespace rx {

// Appends text the way an ostream with default flags writes it, so a writer
// that used to stream into a file produces the same bytes: bool as 1/0, char
// and u8 as characters, integers in decimal, floating point as printf's %g
// with six significant digits. No manipulators; nothing here used them.
class TextWriter {
 public:
  TextWriter& operator<<(base::StringRef s) {
    out_.append(s.data(), s.size());
    return *this;
  }
  TextWriter& operator<<(const char* s) {
    out_.append(s);
    return *this;
  }
  TextWriter& operator<<(const base::String& s) {
    out_.append(s);
    return *this;
  }
  TextWriter& operator<<(char c) {
    out_.push_back(c);
    return *this;
  }
  TextWriter& operator<<(signed char c) { return *this << static_cast<char>(c); }
  TextWriter& operator<<(unsigned char c) { return *this << static_cast<char>(c); }
  TextWriter& operator<<(bool b) { return *this << (b ? '1' : '0'); }
  TextWriter& operator<<(short v) { return Number(v); }
  TextWriter& operator<<(unsigned short v) { return Number(v); }
  TextWriter& operator<<(int v) { return Number(v); }
  TextWriter& operator<<(unsigned int v) { return Number(v); }
  TextWriter& operator<<(long v) { return Number(v); }
  TextWriter& operator<<(unsigned long v) { return Number(v); }
  TextWriter& operator<<(long long v) { return Number(v); }
  TextWriter& operator<<(unsigned long long v) { return Number(v); }
  TextWriter& operator<<(float v) { return Number(static_cast<double>(v)); }
  TextWriter& operator<<(double v) { return Number(v); }

  const base::String& str() const { return out_; }
  base::String Take() { return base::move(out_); }

 private:
  template <typename T>
  TextWriter& Number(T v) {
    base::FormatTo(out_, "{}", v);
    return *this;
  }

  base::String out_;
};

}  // namespace rx

#endif  // RX_CORE_TEXT_WRITER_H_
