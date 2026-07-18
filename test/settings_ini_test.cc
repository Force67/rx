#include "render/core/settings_ini.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition)
    return;
  std::fprintf(stderr, "settings_ini_test: FAIL: %s\n", message);
  ++failures;
}

}  // namespace

int main() {
  rx::render::RenderSettings source;
  source.procedural_grass = false;
  const std::string ini = rx::render::SettingsToIni(source);
  Check(ini.find("procedural_grass = false") != std::string::npos,
        "serialization emits the disabled grass toggle");

  rx::render::RenderSettings round_trip;
  round_trip.procedural_grass = true;
  Check(rx::render::ApplyIni(ini, round_trip) > 0, "serialized preset parses");
  Check(!round_trip.procedural_grass, "grass toggle survives a round trip");

  rx::render::RenderSettings enabled;
  enabled.procedural_grass = false;
  Check(rx::render::ApplyIni("[geometry]\nprocedural_grass = YES\n", enabled) == 1,
        "boolean alias is counted as one applied key");
  Check(enabled.procedural_grass, "boolean alias enables grass");

  rx::render::RenderSettings partial;
  partial.procedural_grass = false;
  rx::render::ApplyIni("[geometry]\nvsync = true\n", partial);
  Check(!partial.procedural_grass, "missing grass key preserves the incoming value");

  rx::render::RenderSettings invalid;
  invalid.procedural_grass = false;
  Check(rx::render::ApplyIni("procedural_grass = sometimes\n", invalid) == 0,
        "invalid boolean is not applied");
  Check(!invalid.procedural_grass, "invalid boolean preserves the incoming value");

  if (failures != 0)
    return 1;
  std::printf("settings_ini_test: PASS\n");
  return 0;
}
