#include "render/core/settings_ini.h"

#include <cmath>
#include <cstdio>
#include <locale>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition)
    return;
  std::fprintf(stderr, "settings_ini_test: FAIL: %s\n", message);
  ++failures;
}

class CommaDecimal final : public std::numpunct<char> {
protected:
  char do_decimal_point() const override { return ','; }
};

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
  Check(std::isfinite(settings.cloud_coverage) &&
            settings.cloud_coverage == 0.5f,
        "a rejected float leaves the setting unchanged");

  std::locale original = std::locale();
  std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
  settings.cloud_coverage = 0.25f;
  std::string serialized = rx::render::SettingsToIni(settings);
  rx::render::RenderSettings restored;
  int round_trip_count = rx::render::ApplyIni(serialized, restored);
  std::locale::global(original);
  Check(round_trip_count > 0 &&
            restored.cloud_coverage == settings.cloud_coverage,
        "serialized settings round-trip under a localized global locale");

  rx::render::RenderSettings source;
  source.procedural_grass = false;
  source.tonemap = rx::render::TonemapOperator::kAgx;
  const std::string ini = rx::render::SettingsToIni(source);
  Check(ini.find("procedural_grass = false") != std::string::npos,
        "serialization emits the disabled grass toggle");
  Check(ini.find("tonemap = agx") != std::string::npos,
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

  if (failures != 0)
    return 1;
  std::printf("settings_ini_test: PASS\n");
  return 0;
}
