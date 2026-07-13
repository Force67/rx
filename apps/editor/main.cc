// rx editor entry point. Boots app::Host with the editor Application. An
// optional .rxscene (or .gltf, partial) path may be passed to open on start;
// otherwise a small built-in default scene keeps the editor from being empty.
#include <string>

#include <base/option.h>

#include "app/host.h"
#include "editor_app.h"

int main(int argc, char** argv) {
  std::string open_path;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (!a.empty() && a[0] != '-') open_path = a;
  }

#if defined(RX_SHARED_BUILD)
  rx::base::InitOptionsFromEnv();
#endif

  rx::app::AppConfig config;
  config.headless = false;
  // The editor emits its own draws in OnBuildView (parent transforms + selection
  // tint), so the host must not also auto-gather them.
  config.gather_entity_draws = false;

  rx::editor::Editor editor(open_path);
  rx::app::Host host;
  if (!host.Initialize(config, editor)) return 1;
  int rc = host.Run();
  host.Shutdown();
  return rc;
}
