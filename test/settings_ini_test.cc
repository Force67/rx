#include "base/strings/xstring.h"
#include "render/core/settings_ini.h"

#include <math.h>
#include <stdio.h>
#include <locale.h>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition)
    return;
  ::fprintf(stderr, "settings_ini_test: FAIL: %s\n", message);
  ++failures;
}

}  // namespace

int main() {
  rx::render::RenderSettings settings;
  settings.cloudscape_steps = 48;

  Check(rx::render::ApplyIni("cloudscape_steps = -1", settings) == 0,
        "negative unsigned values are rejected");
  Check(settings.cloudscape_steps == 48,
        "a rejected value leaves the setting unchanged");
  Check(rx::render::ApplyIni("cloudscape_steps = 4294967296", settings) == 0,
        "overflowing unsigned values are rejected");
  Check(rx::render::ApplyIni("cloudscape_steps = 64junk", settings) == 0,
        "trailing characters are rejected");
  Check(rx::render::ApplyIni("cloudscape_steps = 64", settings) == 1 &&
            settings.cloudscape_steps == 64,
        "a valid unsigned value is applied");

  settings.cloud_coverage = 0.5f;
  Check(rx::render::ApplyIni("cloud_coverage = nan", settings) == 0,
        "non-finite floats are rejected");
  Check(isfinite(settings.cloud_coverage) &&
            settings.cloud_coverage == 0.5f,
        "a rejected float leaves the setting unchanged");

  // A host that localizes the process to a comma decimal point must not change
  // what the ini writes or reads. Skipped when no such locale is installed.
  const char* const kCommaLocales[] = {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "fr_FR.utf8"};
  const char* localized = nullptr;
  for (const char* name : kCommaLocales) {
    if (::setlocale(LC_NUMERIC, name) && ::localeconv()->decimal_point[0] == ',') {
      localized = name;
      break;
    }
  }
  if (localized) {
    settings.cloud_coverage = 0.25f;
    base::String serialized = rx::render::SettingsToIni(settings);
    rx::render::RenderSettings restored;
    int round_trip_count = rx::render::ApplyIni(serialized, restored);
    ::setlocale(LC_NUMERIC, "C");
    Check(round_trip_count > 0 &&
              restored.cloud_coverage == settings.cloud_coverage,
          "serialized settings round-trip under a comma-decimal C locale");
  } else {
    ::fprintf(stderr, "settings_ini_test: no comma-decimal locale installed, locale check skipped\n");
  }

  rx::render::RenderSettings source;
  source.procedural_grass = false;
  source.tonemap = rx::render::TonemapOperator::kAgx;
  const base::String ini = rx::render::SettingsToIni(source);
  Check(ini.find("procedural_grass = false") != base::String::npos,
        "serialization emits the disabled grass toggle");
  Check(ini.find("tonemap = agx") != base::String::npos,
        "serialization emits the AgX tonemap operator");

  rx::render::RenderSettings round_trip;
  round_trip.procedural_grass = true;
  Check(rx::render::ApplyIni(ini, round_trip) > 0,
        "serialized preset parses");
  Check(!round_trip.procedural_grass, "grass toggle survives a round trip");
  Check(round_trip.tonemap == rx::render::TonemapOperator::kAgx,
        "AgX tonemap operator survives a round trip");

  rx::render::RenderSettings enabled;
  enabled.procedural_grass = false;
  Check(rx::render::ApplyIni("[geometry]\nprocedural_grass = YES\n", enabled) ==
            1,
        "boolean alias is counted as one applied key");
  Check(enabled.procedural_grass, "boolean alias enables grass");

  rx::render::RenderSettings partial;
  partial.procedural_grass = false;
  rx::render::ApplyIni("[geometry]\nvsync = true\n", partial);
  Check(!partial.procedural_grass,
        "missing grass key preserves the incoming value");

  rx::render::RenderSettings invalid;
  invalid.procedural_grass = false;
  Check(rx::render::ApplyIni("procedural_grass = sometimes\n", invalid) == 0,
        "invalid boolean is not applied");
  Check(!invalid.procedural_grass,
        "invalid boolean preserves the incoming value");

  // Keys a handheld profile needs to reach: resolution, froxel fog, post.
  rx::render::RenderSettings deck;
  Check(rx::render::ApplyIni("render_scale = 0.75\ndynamic_resolution = true\n"
                             "froxel_fog = false\nmotion_blur = off\nsss = no\n",
                             deck) == 5,
        "resolution, froxel and post keys are recognized");
  Check(deck.render_scale == 0.75f && deck.dynamic_resolution && !deck.froxel_fog &&
            !deck.motion_blur && !deck.sss,
        "resolution, froxel and post keys are applied");
  rx::render::RenderSettings deck_round_trip;
  rx::render::ApplyIni(rx::render::SettingsToIni(deck), deck_round_trip);
  Check(deck_round_trip.render_scale == 0.75f && !deck_round_trip.froxel_fog &&
            !deck_round_trip.motion_blur && !deck_round_trip.sss,
        "the new keys round-trip through SettingsToIni");

  // include: the named file applies first, the including file's keys win, a
  // missing or cyclic include is skipped without taking the file down with it.
  auto resolve = [](base::StringRef name, base::String* text) {
    if (name == "base") {
      *text = "shadow_resolution = 1024\nbloom = false\nssr = false\n";
      return true;
    }
    if (name == "loop") {
      *text = "include = loop\n";
      return true;
    }
    return false;
  };
  rx::render::RenderSettings inc;
  Check(rx::render::ApplyIni("include = base\nbloom = true\n", inc, resolve) == 2,
        "a resolved include counts as one applied line");
  Check(inc.shadow_resolution == 1024 && !inc.ssr, "included keys apply");
  Check(inc.bloom, "the including file's keys override the included ones");
  rx::render::RenderSettings missing;
  Check(rx::render::ApplyIni("include = nope\nssr = false\n", missing, resolve) == 1 &&
            !missing.ssr,
        "an unresolved include is skipped, the rest still applies");
  rx::render::RenderSettings cyclic;
  Check(rx::render::ApplyIni("include = loop\nssr = false\n", cyclic, resolve) == 2 &&
            !cyclic.ssr,
        "a cyclic include stops at the depth limit");

  if (failures != 0)
    return 1;
  ::printf("settings_ini_test: PASS\n");
  return 0;
}
