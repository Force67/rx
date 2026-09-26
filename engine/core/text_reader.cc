#include "core/text_reader.h"

#include <locale.h>
#include <stdlib.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace rx {
namespace {

#if defined(_WIN32)
_locale_t CLocale() {
  static const _locale_t locale = _create_locale(LC_NUMERIC, "C");
  return locale;
}
#else
locale_t CLocale() {
  static const locale_t locale = newlocale(LC_NUMERIC_MASK, "C", static_cast<locale_t>(0));
  return locale;
}
#endif

}  // namespace

f32 StrToF32(const char* text, char** end) {
#if defined(_WIN32)
  return _strtof_l(text, end, CLocale());
#else
  return strtof_l(text, end, CLocale());
#endif
}

f64 StrToF64(const char* text, char** end) {
#if defined(_WIN32)
  return _strtod_l(text, end, CLocale());
#else
  return strtod_l(text, end, CLocale());
#endif
}

}  // namespace rx
