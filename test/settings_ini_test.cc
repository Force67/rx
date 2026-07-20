#include "render/core/settings_ini.h"

#include <cmath>
#include <cstdio>
#include <locale>

namespace {

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition) return;
  std::fprintf(stderr, "settings_ini_test: FAIL: %s\n", message);
  ++failures;
}

class CommaDecimal final : public std::numpunct<char> {
protected:
  char do_decimal_point() const override { return ','; }
};

} // namespace

int main() {
  rx::render::RenderSettings settings;
  settings.cloudscape_steps = 48;

  Check(rx::render::ApplyIni("cloudscape_steps = -1", settings) == 0,
        "negative unsigned values are rejected");
  Check(settings.cloudscape_steps == 48, "a rejected value leaves the setting unchanged");
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
  Check(std::isfinite(settings.cloud_coverage) && settings.cloud_coverage == 0.5f,
        "a rejected float leaves the setting unchanged");

  std::locale original = std::locale();
  std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
  settings.cloud_coverage = 0.25f;
  std::string serialized = rx::render::SettingsToIni(settings);
  rx::render::RenderSettings restored;
  int round_trip = rx::render::ApplyIni(serialized, restored);
  std::locale::global(original);
  Check(round_trip > 0 && restored.cloud_coverage == settings.cloud_coverage,
        "serialized settings round-trip under a localized global locale");

  if (failures == 0) {
    std::printf("settings_ini_test: OK\n");
    return 0;
  }
  std::fprintf(stderr, "settings_ini_test: %d failure(s)\n", failures);
  return failures;
}
