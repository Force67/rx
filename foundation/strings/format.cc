#include "foundation/strings/format.h"

#include <math.h>
#include <stdlib.h>

#include "base/strings/float_format.h"

namespace rx {
namespace {

// |value| as count significant decimal digits times 10^exponent, the first
// digit ahead of the point: 1.25e-3 is {"125", 3, -3}.
struct Decimal {
  char digits[24];
  int count = 0;
  int exponent = 0;
};

// The correctly rounded |precision|-digit decimal of a finite positive value,
// taken apart from base's exact %e.
Decimal Round(f64 value, int precision) {
  // FormatFloatTo does not terminate its output; atoi below must not run on
  // into whatever the stack holds after the exponent.
  char text[64];
  const mem_size n =
      base::FormatFloatTo(text, sizeof(text) - 1, value, 'e', precision - 1, '\0', false);
  text[n < sizeof(text) ? n : sizeof(text) - 1] = '\0';
  Decimal d;
  mem_size i = 0;
  for (; i < n && text[i] != 'e'; ++i) {
    if (text[i] != '.') d.digits[d.count++] = text[i];
  }
  d.exponent = atoi(text + i + 1);
  return d;
}

// Moves the last digit one step up or down, carrying, at the same length.
Decimal Step(Decimal d, int direction) {
  int i = d.count - 1;
  if (direction > 0) {
    while (i >= 0 && d.digits[i] == '9') d.digits[i--] = '0';
    if (i < 0) {
      // 9.99 -> 10.0: renormalize to 1.00 one decade up.
      d.digits[0] = '1';
      for (int k = 1; k < d.count; ++k) d.digits[k] = '0';
      ++d.exponent;
    } else {
      ++d.digits[i];
    }
  } else {
    while (i >= 0 && d.digits[i] == '0') d.digits[i--] = '9';
    --d.digits[i];
    if (d.digits[0] == '0') {
      // 1.00 -> 0.99(9): renormalize to 9.99 one decade down.
      for (int k = 0; k + 1 < d.count; ++k) d.digits[k] = d.digits[k + 1];
      d.digits[d.count - 1] = '9';
      --d.exponent;
    }
  }
  return d;
}

// "d.ddde[-]x", enough for strtod.
void Scientific(const Decimal& d, char* out) {
  int n = 0;
  out[n++] = d.digits[0];
  if (d.count > 1) {
    out[n++] = '.';
    for (int k = 1; k < d.count; ++k) out[n++] = d.digits[k];
  }
  out[n++] = 'e';
  int e = d.exponent;
  if (e < 0) {
    out[n++] = '-';
    e = -e;
  }
  char rev[8];
  int r = 0;
  do {
    rev[r++] = static_cast<char>('0' + e % 10);
    e /= 10;
  } while (e);
  while (r) out[n++] = rev[--r];
  out[n] = '\0';
}

template <typename T>
bool ReadsBackAs(const Decimal& d, T value) {
  char text[48];
  Scientific(d, text);
  if constexpr (sizeof(T) == sizeof(f32)) {
    return ::strtof(text, nullptr) == value;
  } else {
    return ::strtod(text, nullptr) == value;
  }
}

template <typename T>
Decimal Shortest(T value) {
  constexpr int kMaxDigits = sizeof(T) == sizeof(f32) ? 9 : 17;
  for (int p = 1; p < kMaxDigits; ++p) {
    const Decimal nearest = Round(value, p);
    if (ReadsBackAs(nearest, value)) return nearest;
    // At a power of two the rounding interval is narrower below than above,
    // so the nearest p-digit decimal can miss while the one on the far side
    // of |value| still reads back. No other p-digit decimal can.
    char text[48];
    Scientific(nearest, text);
    const bool nearest_below = ::strtod(text, nullptr) < static_cast<f64>(value);
    const Decimal other = Step(nearest, nearest_below ? 1 : -1);
    if (ReadsBackAs(other, value)) return other;
  }
  return Round(value, kMaxDigits);
}

void AppendDecimal(base::String& out, Decimal d, f64 value) {
  while (d.count > 1 && d.digits[d.count - 1] == '0') --d.count;

  // printf's %e: at least two exponent digits.
  char sci[48];
  int s = 0;
  sci[s++] = d.digits[0];
  if (d.count > 1) {
    sci[s++] = '.';
    for (int k = 1; k < d.count; ++k) sci[s++] = d.digits[k];
  }
  sci[s++] = 'e';
  int e = d.exponent;
  sci[s++] = e < 0 ? '-' : '+';
  if (e < 0) e = -e;
  char rev[8];
  int r = 0;
  do {
    rev[r++] = static_cast<char>('0' + e % 10);
    e /= 10;
  } while (e);
  if (r < 2) rev[r++] = '0';
  while (r) sci[s++] = rev[--r];

  // %f of the same digits: the point after exponent + 1 digits.
  const int point = d.exponent + 1;
  // With no fraction the value is an integer, and the fixed form is its exact
  // digits: every same-length spelling reads back, and std picks the closest.
  char whole[400];
  int fixed_length;
  if (point <= 0) {
    fixed_length = 2 - point + d.count;  // "0." zeros digits
  } else if (point >= d.count) {
    fixed_length = static_cast<int>(
        base::FormatFloatTo(whole, sizeof(whole), value, 'f', 0, '\0', false));
  } else {
    fixed_length = d.count + 1;
  }

  if (s < fixed_length) {
    out.append(sci, static_cast<mem_size>(s));
    return;
  }
  if (point <= 0) {
    out.append("0.");
    for (int k = 0; k < -point; ++k) out.push_back('0');
    out.append(d.digits, static_cast<mem_size>(d.count));
  } else if (point >= d.count) {
    out.append(whole, static_cast<mem_size>(fixed_length));
  } else {
    out.append(d.digits, static_cast<mem_size>(point));
    out.push_back('.');
    out.append(d.digits + point, static_cast<mem_size>(d.count - point));
  }
}

template <typename T>
void AppendShortestImpl(base::String& out, T value) {
  if (::isnan(value)) {
    out.append(::signbit(value) ? "-nan" : "nan");
    return;
  }
  if (::signbit(value)) {
    out.push_back('-');
    value = -value;
  }
  if (::isinf(value)) {
    out.append("inf");
    return;
  }
  if (value == 0) {
    out.push_back('0');
    return;
  }
  AppendDecimal(out, Shortest(value), static_cast<f64>(value));
}

}  // namespace

void AppendShortest(base::String& out, f32 value) { AppendShortestImpl(out, value); }
void AppendShortest(base::String& out, f64 value) { AppendShortestImpl(out, value); }

namespace format_detail {

void FormatImpl(base::String& out, const char* fmt, const Appender* appenders,
                const void* const* values, mem_size count) {
  mem_size next_auto = 0;
  const char* run = fmt;
  const char* p = fmt;
  auto flush = [&](const char* end) {
    if (end > run) out.append(run, static_cast<mem_size>(end - run));
  };
  while (*p) {
    if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
      flush(p + 1);
      p += 2;
      run = p;
      continue;
    }
    if (*p != '{') {
      ++p;
      continue;
    }
    flush(p);
    ++p;
    mem_size index = 0;
    bool manual = false;
    while (*p >= '0' && *p <= '9') {
      index = index * 10 + static_cast<mem_size>(*p - '0');
      manual = true;
      ++p;
    }
    if (!manual) index = next_auto++;
    base::StringRef spec;
    if (*p == ':') {
      const char* spec_start = ++p;
      while (*p && *p != '}') ++p;
      spec = base::StringRef(spec_start, static_cast<mem_size>(p - spec_start));
    }
    // The compile-time check rules out a bad index or a missing '}'.
    if (index < count) appenders[index](out, spec, values[index]);
    if (*p == '}') ++p;
    run = p;
  }
  flush(p);
}

}  // namespace format_detail
}  // namespace rx
