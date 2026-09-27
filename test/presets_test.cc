#include "base/strings/xstring.h"
#include "render/core/presets.h"
#include "render/core/settings_ini.h"

#include <stdio.h>
#include <string.h>

#include "render/presets/android_high.h"
#include "render/presets/android_low.h"
#include "render/presets/android_medium.h"
#include "render/presets/console.h"
#include "render/presets/high.h"
#include "render/presets/low.h"
#include "render/presets/medium.h"
#include "render/presets/steamdeck.h"
#include "render/presets/ultra.h"

namespace {

int failures = 0;

void Check(bool condition, const char* what, const char* detail) {
  if (condition) return;
  ::fprintf(stderr, "presets_test: FAIL: %s (%s)\n", what, detail);
  ++failures;
}

// "key = value" lines, the way ApplyIni reads them.
int KeyLines(const char* text, size_t size) {
  int keys = 0;
  size_t i = 0;
  while (i < size) {
    size_t end = i;
    while (end < size && text[end] != '\n') ++end;
    size_t b = i;
    while (b < end && (text[b] == ' ' || text[b] == '\t')) ++b;
    const bool comment = b < end && (text[b] == '#' || text[b] == ';' || text[b] == '[');
    if (b < end && !comment && ::memchr(text + b, '=', end - b)) ++keys;
    i = end + 1;
  }
  return keys;
}

template <size_t N>
void CheckTier(const char* name, const unsigned char (&bytes)[N]) {
  const char* text = reinterpret_cast<const char*>(bytes);
  rx::render::RenderSettings s;
  const int applied = rx::render::ApplyIni(base::StringRef(text, N), s);
  // ApplyIni skips keys it does not know, so a misspelled key in a shipped tier
  // would silently fall back to the default.
  Check(applied == KeyLines(text, N), "every key in the tier ini is recognized", name);
  Check(rx::render::ParsePreset(name) != rx::render::QualityPreset::kAuto,
        "the tier file is named after its tier", name);
}

}  // namespace

int main() {
  CheckTier("android_low", kPreset_android_low);
  CheckTier("android_medium", kPreset_android_medium);
  CheckTier("android_high", kPreset_android_high);
  CheckTier("steamdeck", kPreset_steamdeck);
  CheckTier("low", kPreset_low);
  CheckTier("console", kPreset_console);
  CheckTier("medium", kPreset_medium);
  CheckTier("high", kPreset_high);
  CheckTier("ultra", kPreset_ultra);

  struct Case {
    const char* adapter;
    const char* tier;
  };
  const Case cases[] = {
      {"Adreno (TM) 740", "android_high"},   {"Adreno (TM) 650", "android_medium"},
      {"Adreno (TM) 618", "android_low"},    {"Mali-G715 MC7", "android_high"},
      {"Mali-G610 MC6", "android_medium"},   {"Mali-G78 MP20", "android_medium"},
      {"Mali-G57 MC2", "android_low"},       {"Mali-T880", "android_low"},
      {"Immortalis-G720", "android_high"},   {"Samsung Xclipse 940", "android_high"},
      {"PowerVR Rogue GE8320", "android_low"},
  };
  for (const Case& c : cases) {
    rx::render::DeviceCaps caps{};
    caps.ray_query = true;
    caps.adapter_name = c.adapter;
    Check(::strcmp(rx::render::PresetName(rx::render::DetectPreset(caps)), c.tier) == 0,
          "mobile gpu lands in its tier", c.adapter);
  }

  if (failures != 0) return 1;
  ::printf("presets_test: PASS\n");
  return 0;
}
