#include <cstring>
#include <string>

#include <base/option.h>

#include "app/host.h"
#include "core/log.h"
#include "viewer.h"

namespace {

void PrintUsage() {
  RX_INFO("usage: rx [options]");
  RX_INFO("  --gltf <path>         load a gltf/glb scene (e.g. assets/sponza/Sponza.gltf)");
  RX_INFO("  --demo <id>           builtin scene: water | materials | gaussian | cornell |");
  RX_INFO("                        cloth | locomotion | ship | nav | lod | oit | fire | brick | silpom | sss | scenehook | ... (cube)");
  RX_INFO("  --headless            no window, no renderer");
  RX_INFO("  --preset <tier>       auto (default) | android | steamdeck | low |");
  RX_INFO("                        medium | high | ultra | console");
  RX_INFO("  --no-taa              disable temporal antialiasing");
  RX_INFO("  --upscaler <id>       fsr3 | dlss | xess");
  RX_INFO("  --no-rt               disable raytracing");
  RX_INFO("  --validation          enable vulkan validation layers");
}

rx::render::UpscalerKind ParseUpscaler(const std::string& id) {
  if (id == "fsr3") return rx::render::UpscalerKind::kFsr3;
  if (id == "dlss") return rx::render::UpscalerKind::kDlss;
  if (id == "xess") return rx::render::UpscalerKind::kXess;
  return rx::render::UpscalerKind::kNone;
}

}  // namespace

int main(int argc, char** argv) {
  rx::EngineConfig config;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

    if (arg == "--gltf") config.gltf_path = next();
    else if (arg == "--demo") config.demo_scene = next();
    else if (arg == "--headless") config.headless = true;
    else if (arg == "--preset") config.preset = rx::render::ParsePreset(next());
    else if (arg == "--no-taa") config.renderer.aa_mode = rx::render::AntiAliasingMode::kNone;
    else if (arg == "--upscaler") config.renderer.upscaler = ParseUpscaler(next());
    else if (arg == "--no-rt") config.renderer.enable_raytracing = false;
    else if (arg == "--validation") config.renderer.enable_validation = true;
    else {
      PrintUsage();
      return arg == "--help" ? 0 : 1;
    }
  }

  rx::app::AppConfig app_config;
  app_config.renderer = config.renderer;
  app_config.preset = config.preset;
  app_config.headless = config.headless;

#if defined(RX_SHARED_BUILD)
  // The viewer's own base::Option knobs (viewer.cc, camera_input.cc,
  // demo_scenes.cc, debug_ui.cc) live on this executable's InitChain, which
  // under RX_SHARED is a separate instance from the engine DSOs' chains. Apply
  // the executable's env overrides here; each engine DSO applies its own at
  // subsystem init. Compiled out in the static build (one shared chain).
  base::InitOptionsFromEnv();
#endif

  rx::Viewer viewer(config);
  rx::app::Host host;
  if (!host.Initialize(app_config, viewer)) {
    RX_ERROR("engine initialization failed");
    return 1;
  }
  int rc = host.Run();
  host.Shutdown();
  return rc;
}
