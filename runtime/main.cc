#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <base/option.h>

#include "app/host.h"
#include "core/log.h"
#include "edit/reflect.h"
#include "scene/scene_handlers.h"
#include "scene_authoring.h"
#include "scene_validate.h"
#include "script/handler_registry.h"
#include "script/script_value.h"
#include "viewer.h"

namespace {

// The delta a capture run advances by. 1/60 s, which is FrameTimer's own fixed
// step, so the accumulator hands out exactly one sim step per frame instead of
// a 0-or-2 pattern that makes the capture frame land mid-step; it is also what
// the demos mirroring the host's cadence (demo_gym, demo_shooter) fall back to.
constexpr float kCaptureDelta = 1.0f / 60.0f;

void PrintUsage() {
  RX_INFO("usage: rx [options]");
  RX_INFO("  --gltf <path>         load a gltf/glb scene (e.g. assets/sponza/Sponza.gltf)");
  RX_INFO("  --usd <path>          load a usd/usda/usdc/usdz stage");
  RX_INFO("  --scene <path>        load any of the above, or a .rxscene text scene");
  RX_INFO("  --usd-show <prim>     force a usd prim subtree visible (repeatable)");
  RX_INFO("  --usd-hide <prim>     force a usd prim subtree hidden (repeatable)");
  RX_INFO("  --demo <id>           builtin scene: water | fluid | weather | materials | gaussian | cornell |");
  RX_INFO("                        featuregym | cloth | locomotion | ship | nav | gym | shooter | puppet | drive |");
  RX_INFO("                        placement | grass | lod | oit | fire | brick | silpom | sss | scenehook | ... (cube)");
  RX_INFO("  --dump-schema         print the .rxscene component schema as json and exit");
  RX_INFO("  --dump-commands       print the live command schema as json and exit");
  RX_INFO("  --validate <path>     structurally check a .rxscene (no gpu); nonzero exit on an");
  RX_INFO("                        error-level finding. --json for a machine-readable report");
  RX_INFO("  --json                emit --validate's report as json instead of text");
  RX_INFO("  --authoring-endpoint <path>  serve those commands on a local unix socket");
  RX_INFO("  --headless            no window (a --shot run still brings the gpu up, windowless)");
  RX_INFO("  --shot <path.png>     capture a frame, then quit; nonzero exit if it was not written.");
  RX_INFO("                        Runs the clock in lockstep at 1/60 s so the png does not depend");
  RX_INFO("                        on machine load (RX_FIXED_DT overrides, 0 = wall clock)");
  RX_INFO("  --shot-frames <n>     frames to render before the capture (default 30)");
  RX_INFO("  --width <px>          render/window width (also RX_WIN_W)");
  RX_INFO("  --height <px>         render/window height (also RX_WIN_H)");
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

void PrintJsonString(const char* s) {
  std::putchar('"');
  for (const char* c = s; *c; ++c) {
    if (*c == '"' || *c == '\\') std::putchar('\\');
    std::putchar(*c);
  }
  std::putchar('"');
}

// The .rxscene authoring surface, generated from the live reflection registry
// so it can never drift from what the loader accepts. This is the API doc for
// anything writing scene files by hand.
void DumpSchema() {
  rx::RegisterSceneComponents();
  std::printf("{\n  \"components\": [\n");
  const auto components = rx::edit::AllComponents();
  for (size_t i = 0; i < components.size(); ++i) {
    const rx::edit::ComponentDesc& comp = *components[i];
    std::printf("    {\n      \"name\": ");
    PrintJsonString(comp.name);
    std::printf(",\n      \"props\": [");
    for (rx::u32 p = 0; p < comp.prop_count; ++p) {
      const rx::edit::PropDesc& prop = comp.props[p];
      std::printf("%s\n        {\"name\": ", p ? "," : "");
      PrintJsonString(prop.name);
      std::printf(", \"type\": ");
      PrintJsonString(rx::edit::PropTypeName(prop.type));
      // 0/0 is "unbounded" (PropDesc), so only a real Range prints.
      if (prop.min != 0.0f || prop.max != 0.0f)
        std::printf(", \"min\": %g, \"max\": %g", prop.min, prop.max);
      if (prop.hint) {
        std::printf(", \"hint\": ");
        PrintJsonString(prop.hint);
      }
      std::printf("}");
    }
    std::printf("%s]\n    }%s\n", comp.prop_count ? "\n      " : "",
                i + 1 < components.size() ? "," : "");
  }
  std::printf("  ]\n}\n");
}

// The live command surface (--authoring-endpoint), generated from the registry
// the endpoint dispatches into, so a caller reads the same signatures the bridge
// enforces. `wire_args` is what the transport actually carries: a vec3 param
// travels as three numbers, so it is not always the length of `params`.
void DumpCommands() {
  rx::script::HandlerRegistry commands;
  rx::scene::SetupSceneCommands(commands);
  std::printf("{\n  \"commands\": [\n");
  for (size_t i = 0; i < commands.size(); ++i) {
    const rx::script::HandlerDesc& desc = commands.at(i);
    std::printf("    {\"name\": ");
    PrintJsonString(std::string(desc.name.view()).c_str());
    std::printf(", \"params\": [");
    rx::u32 wire_args = 0;
    for (rx::u32 p = 0; p < desc.sig.count; ++p) {
      const rx::script::ScriptType type = desc.sig.params[p];
      wire_args += type == rx::script::ScriptType::kVec3 ? 3 : 1;
      std::printf("%s", p ? ", " : "");
      PrintJsonString(rx::script::ScriptTypeName(type));
    }
    std::printf("], \"wire_args\": %u, \"returns\": ", wire_args);
    PrintJsonString(rx::script::ScriptTypeName(desc.sig.ret));
    std::printf("}%s\n", i + 1 < commands.size() ? "," : "");
  }
  std::printf("  ]\n}\n");
}

}  // namespace

int main(int argc, char** argv) {
  rx::EngineConfig config;
  rx::app::AppConfig app_config;
  bool no_window = false;
  bool dump_schema = false;
  bool dump_commands = false;
  std::string validate_path;
  bool json = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

    if (arg == "--gltf" || arg == "--usd" || arg == "--scene") config.scene_path = next();
    else if (arg == "--demo") config.demo_scene = next();
    else if (arg == "--usd-show") config.usd_visibility.show.push_back(next());
    else if (arg == "--usd-hide") config.usd_visibility.hide.push_back(next());
    else if (arg == "--dump-schema") dump_schema = true;
    else if (arg == "--dump-commands") dump_commands = true;
    else if (arg == "--validate") validate_path = next();
    else if (arg == "--json") json = true;
    else if (arg == "--authoring-endpoint") config.authoring_socket = next();
    else if (arg == "--headless") no_window = true;
    else if (arg == "--shot") config.shot_path = next();
    else if (arg == "--shot-frames") config.shot_frames = std::atoi(next().c_str());
    else if (arg == "--width") app_config.width = static_cast<rx::u32>(std::atoi(next().c_str()));
    else if (arg == "--height") app_config.height = static_cast<rx::u32>(std::atoi(next().c_str()));
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

  // Pure queries: no window, no device, nothing to tear down.
  if (dump_schema) {
    DumpSchema();
    return 0;
  }
  if (dump_commands) {
    DumpCommands();
    return 0;
  }
  // Same category: the structural checks touch no device and no window, which
  // is what lets this run on every edit and in CI.
  if (!validate_path.empty()) return rx::ValidateSceneFile(validate_path, json) ? 0 : 1;

  if (config.demo_scene == "featuregym" || config.demo_scene == "feature-gym") {
    config.renderer.software_gi_fallback = true;
    if (!config.renderer.enable_raytracing) config.renderer.software_gi = true;
  }

  // The env var is the older spelling of --shot and still drives existing
  // capture scripts; resolve both here because whether a shot is armed decides
  // whether a windowless run needs the gpu at all.
  std::string shot = config.shot_path;
  if (shot.empty()) {
    if (const char* env = std::getenv("RX_UI_SHOT")) shot = env;
  }
  config.offscreen = no_window && !shot.empty();
  config.headless = no_window && !config.offscreen;

  app_config.renderer = config.renderer;
  app_config.preset = config.preset;
  app_config.headless = no_window;
  app_config.offscreen = config.offscreen;
  // A capture exists to be compared against another capture, so it must not
  // depend on how fast this machine happened to run the frames before it: lock
  // the clock, and the png is a function of the frame index. This is on by
  // default because forgetting it does not fail, it just quietly widens the
  // run-to-run noise (rmse 0.0002 -> 0.008, the size of a real regression, so a
  // comparison then catches nothing); RX_FIXED_DT=0 asks for the wall clock
  // back and RX_FIXED_DT=<seconds> picks a different delta.
  if (!shot.empty()) app_config.fixed_delta = kCaptureDelta;

  // A stale png from an earlier run would otherwise pass the check below.
  const bool verify_shot = !config.shot_path.empty() && !std::getenv("RX_UI_SHOT_SEQ");
  if (verify_shot) std::filesystem::remove(config.shot_path);

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

  // The capture is the whole point of a --shot run, so its absence (no device,
  // a scene that rendered nothing, a quit before the write landed) has to be
  // visible in the exit code rather than only in the log.
  if (rc == 0 && verify_shot && !std::filesystem::exists(config.shot_path)) {
    RX_ERROR("no screenshot written to '{}'", config.shot_path);
    return 1;
  }
  return rc;
}
