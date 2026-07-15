#include "debug_ui.h"

#include "fly_camera.h"

#if defined(RX_HAS_IMGUI)

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <base/option.h>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include "core/log.h"
#include "render/core/presets.h"
#include "render/core/settings_ini.h"

#ifndef RX_BUILD_ID
#define RX_BUILD_ID "unknown"
#endif
#ifndef RX_BUILD_CONFIG
#define RX_BUILD_CONFIG "unknown"
#endif
#ifndef RX_VERSION
#define RX_VERSION "unknown"
#endif

namespace rx {
namespace {

// Config toggle formerly read from getenv (populated by base::InitOptionsFromEnv).
base::Option<bool> HideDebugUi{"hide.debug.ui", false, "RX_HIDE_DEBUG_UI"};

// Override for the editable .ini render presets directory; defaults to the
// compiled-in engine/render/presets source path.
base::Option<const char*> PresetsDirOpt{"presets.dir", nullptr, "RX_PRESETS_DIR"};

// Directory holding the .ini render presets: RX_PRESETS_DIR, else the
// compiled-in source path, else a cwd-relative fallback.
std::filesystem::path PresetDir() {
  if (const char* env = PresetsDirOpt.get(); env && *env) return env;
#ifdef RX_PRESETS_DIR_DEFAULT
  return std::filesystem::path(RX_PRESETS_DIR_DEFAULT);
#else
  return std::filesystem::path("engine/render/presets");
#endif
}

const char* kAaModes[] = {"None", "TAA", "FSR3 Upscaler", "DLSS Upscaler"};
const char* kQualities[] = {"Native AA (1.0x)", "Quality (1.5x)", "Balanced (1.7x)",
                            "Performance (2.0x)"};
const char* kTonemaps[] = {"ACES", "Reinhard", "None", "AgX"};
const char* kColorGrades[] = {"Neutral", "Warm", "Cool", "Cinematic", "Custom (.cube)"};
const char* kDebugViews[] = {"Off",         "Base color",   "World normal",
                             "Roughness",   "Metallic",     "Ambient occlusion",
                             "Indirect GI", "Direct light", "Emissive", "Reflection",
                             "Overdraw",    "Bounds (BVH)", "Temporal history",
                             "Motion vectors", "Ray count", "Light complexity"};

// FPS readout colour bands: green above smooth, amber in the warning band, red
// once it slips below playable.
constexpr f32 kFpsGood = 60.0f;
constexpr f32 kFpsWarn = 30.0f;
constexpr f32 kStatusBarHeight = 42.0f;
constexpr f32 kStatusGraphMaxFps = 120.0f;

#if defined(NDEBUG)
constexpr const char* kBuildMode = "RELEASE";
#else
constexpr const char* kBuildMode = "DEBUG";
#endif

// Row 0 is "Custom" (hand-tuned); the rest map to QualityPreset below.
const char* kPresets[] = {"Custom",  "Auto-detect", "Android", "Steam Deck", "Low end",
                          "Console", "Medium",      "High",    "Ultra"};
const render::QualityPreset kPresetValues[] = {
    render::QualityPreset::kAuto,      // unused for row 0
    render::QualityPreset::kAuto,      render::QualityPreset::kAndroid,
    render::QualityPreset::kSteamDeck, render::QualityPreset::kLowEnd,
    render::QualityPreset::kConsole,   render::QualityPreset::kMedium,
    render::QualityPreset::kHigh,      render::QualityPreset::kUltra};

}  // namespace

DebugUi::DebugUi() = default;
DebugUi::~DebugUi() { Shutdown(); }

bool DebugUi::Initialize(Window& window, render::Renderer& renderer) {
  SDL_Window* sdl_window = static_cast<SDL_Window*>(window.native_handles().window);
  render::Device* device = renderer.device();
  if (!sdl_window || !device || device->is_stub()) return false;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
  io.IniFilename = nullptr;  // no imgui.ini litter next to the binary
  // The RHI render backend honors dynamic textures and large-mesh vertex
  // offsets; it sets no imgui globals itself (RX_SHARED keeps one context in
  // this app DSO), so the flags are the app's to set.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
  ImGui::StyleColorsDark();

  io.Fonts->AddFontDefault();

  if (!ImGui_ImplSDL3_InitForVulkan(sdl_window)) return false;

  if (!imgui_renderer_.Initialize(*device, renderer.swapchain_format())) {
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    return false;
  }

  window.set_event_hook([](const void* event) {
    ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(event));
  });

  // RX_HIDE_DEBUG_UI starts with the imgui overlays hidden, so the libultragui
  // HUD has the screen to itself for clean screenshots (cf. RX_UI_MENU).
  if (HideDebugUi) visible_ = false;
  window_ = &window;
  initialized_ = true;
  RX_INFO("imgui {} initialized (rhi render backend)", IMGUI_VERSION);
  return true;
}

void DebugUi::Shutdown() {
  if (!initialized_) return;
  imgui_renderer_.Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  initialized_ = false;
}

void DebugUi::BeginFrame() {
  if (!initialized_) return;
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();
}

bool DebugUi::wants_mouse() const {
  return initialized_ && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

bool DebugUi::wants_keyboard() const {
  return initialized_ && ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureKeyboard;
}

void DebugUi::Build(render::Renderer& renderer, FlyCamera& camera, f32 frame_delta,
                    render::FrameView* view) {
  if (!initialized_) return;

  frame_times_[frame_time_cursor_] = frame_delta * 1000.0f;
  frame_time_cursor_ = (frame_time_cursor_ + 1) % IM_ARRAYSIZE(frame_times_);
  frame_time_count_ = std::min<u32>(frame_time_count_ + 1, IM_ARRAYSIZE(frame_times_));

  // Per-pass GPU timestamps cost real frame time (barriers per pass), so the
  // renderer only records them while this overlay displays them (the GPU
  // passes table and the stage chart). The boot value (RX_GPU_TIMINGS) is
  // latched once so a forced-on headless run keeps them without the overlay.
  if (!gpu_timings_latched_) {
    gpu_timings_forced_ = renderer.settings().gpu_pass_timings;
    gpu_timings_latched_ = true;
  }
  renderer.settings().gpu_pass_timings = visible_ || gpu_timings_forced_;

  if (visible_) {
    render::RenderSettings& settings = renderer.settings();
    const render::DeviceCaps* caps = renderer.caps();

    ImGui::SetNextWindowPos({16, 16}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({460, 640}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Renderer (F1 hides)")) {
      if (caps) ImGui::TextWrapped("%s", caps->adapter_name.c_str());
      ImGui::Text("output %ux%u  render %ux%u", renderer.output_width(),
                  renderer.output_height(), renderer.render_width(), renderer.render_height());
      // The fps figure turns amber then red as it drops past the thresholds.
      const f32 fps = frame_delta > 0 ? 1.0f / frame_delta : 0.0f;
      const ImVec4 fps_col = fps >= kFpsGood   ? ImVec4{0.45f, 0.92f, 0.45f, 1.0f}
                             : fps >= kFpsWarn ? ImVec4{0.97f, 0.80f, 0.30f, 1.0f}
                                               : ImVec4{0.97f, 0.33f, 0.28f, 1.0f};
      ImGui::Text("%.2f ms", frame_delta * 1000.0f);
      ImGui::SameLine();
      ImGui::TextColored(fps_col, "(%.0f fps)", fps);
      const int history_offset = frame_time_count_ == IM_ARRAYSIZE(frame_times_)
                                     ? static_cast<int>(frame_time_cursor_)
                                     : 0;
      ImGui::PlotLines("##frametimes", frame_times_, static_cast<int>(frame_time_count_),
                       history_offset, nullptr, 0.0f, 33.3f,
                       {ImGui::GetContentRegionAvail().x, 48});

      // One-click hardware tier: overwrites every feature toggle below. Touching
      // any of them afterwards just leaves this on the chosen row (still custom).
      if (ImGui::Combo("Quality preset", &preset_choice_, kPresets, IM_ARRAYSIZE(kPresets)) &&
          preset_choice_ > 0 && caps) {
        render::QualityPreset preset = kPresetValues[preset_choice_];
        settings = render::PresetSettings(preset, *caps);
        if (preset == render::QualityPreset::kAuto) {
          RX_INFO("preset: auto -> {}", render::PresetName(render::DetectPreset(*caps)));
        }
      }

      // Editable per-platform .ini presets (engine/render/presets). Loaded
      // straight onto the live settings; "for now" the debug ui is the only way
      // in. Save writes the current settings back out so users can author more.
      if (ImGui::CollapsingHeader("Platform preset (.ini)")) {
        if (!preset_files_scanned_) ScanPresetFiles();
        if (preset_files_.empty()) {
          ImGui::TextDisabled("no .ini in %s", PresetDir().string().c_str());
        } else {
          std::vector<const char*> names;
          names.reserve(preset_files_.size());
          for (const auto& f : preset_files_) names.push_back(f.c_str());
          ImGui::Combo("File", &preset_file_choice_, names.data(), static_cast<int>(names.size()));
          if (ImGui::Button("Load")) {
            const auto path = PresetDir() / preset_files_[preset_file_choice_];
            if (render::LoadSettingsIni(path, settings)) {
              preset_choice_ = 0;  // settings are file-tuned now, not a hardware tier
              preset_status_ = "loaded " + preset_files_[preset_file_choice_];
              RX_INFO("render preset: loaded {}", path.string());
            } else {
              preset_status_ = "could not open " + preset_files_[preset_file_choice_];
            }
          }
          ImGui::SameLine();
        }
        if (ImGui::Button("Rescan")) ScanPresetFiles();
        ImGui::SetNextItemWidth(150);
        ImGui::InputText("##presetname", preset_save_name_, sizeof(preset_save_name_));
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
          std::string fn = preset_save_name_[0] ? preset_save_name_ : "custom";
          if (fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".ini") != 0) fn += ".ini";
          const auto path = PresetDir() / fn;
          if (render::SaveSettingsIni(path, settings)) {
            preset_status_ = "saved " + fn;
            RX_INFO("render preset: saved {}", path.string());
            ScanPresetFiles();
          } else {
            preset_status_ = "could not write " + fn;
          }
        }
        if (!preset_status_.empty()) ImGui::TextDisabled("%s", preset_status_.c_str());
      }

      // Per-topic graphics submenus: a tab bar across the top splits the long
      // flat option list into one area of the pipeline each. The presets above
      // stay visible whichever tab is open.
      if (ImGui::BeginTabBar("gfx_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Display")) {
          DrawDisplayTab(renderer, settings);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Ray tracing")) {
          DrawRayTracingTab(renderer, settings, caps);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lighting")) {
          DrawLightingTab(settings, caps);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Global illum.")) {
          DrawGiTab(settings, caps);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Post FX")) {
          DrawPostTab(settings);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics")) {
          DrawDiagnosticsTab(renderer, camera, settings, caps);
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }

    }
    ImGui::End();

    DrawStageChart(renderer, kStatusBarHeight);
    DrawStatusBar(frame_delta, *view);

    if (show_demo_) ImGui::ShowDemoWindow(&show_demo_);
  }

  ImGui::Render();
  if (visible_ || ImGui::GetDrawData()->TotalVtxCount > 0) {
    view->ui_draw = [this](render::CommandList& cmd) {
      imgui_renderer_.Render(ImGui::GetDrawData(), cmd);
    };
  }
}

void DebugUi::DrawDisplayTab(render::Renderer& renderer, render::RenderSettings& settings) {
  int aa = settings.aa_mode == render::AntiAliasingMode::kNone   ? 0
           : settings.aa_mode == render::AntiAliasingMode::kTaa   ? 1
           : settings.upscaler == render::UpscalerKind::kDlss     ? 3
                                                                  : 2;
  if (ImGui::Combo("Mode", &aa, kAaModes, IM_ARRAYSIZE(kAaModes))) {
    switch (aa) {
      case 0:
        settings.aa_mode = render::AntiAliasingMode::kNone;
        settings.upscaler = render::UpscalerKind::kNone;
        break;
      case 1:
        settings.aa_mode = render::AntiAliasingMode::kTaa;
        settings.upscaler = render::UpscalerKind::kNone;
        break;
      case 2:
        settings.upscaler = render::UpscalerKind::kFsr3;
        settings.aa_mode = render::AntiAliasingMode::kUpscaler;
        break;
      case 3:
        settings.upscaler = render::UpscalerKind::kDlss;
        settings.aa_mode = render::AntiAliasingMode::kUpscaler;
        break;
    }
  }
  if (settings.aa_mode == render::AntiAliasingMode::kUpscaler &&
      settings.upscaler != render::UpscalerKind::kNone) {
    int quality = static_cast<int>(settings.upscaler_quality);
    if (ImGui::Combo("Quality", &quality, kQualities, IM_ARRAYSIZE(kQualities))) {
      settings.upscaler_quality = static_cast<render::UpscalerQuality>(quality);
    }
    ImGui::SliderFloat("Sharpness", &settings.sharpness, 0.0f, 1.0f);
    if (!renderer.upscaler_active()) {
      ImGui::TextColored({1, 0.6f, 0.3f, 1}, "upscaler unavailable, taa fallback");
    }
  }
  if (settings.aa_mode == render::AntiAliasingMode::kTaa) {
    ImGui::SliderFloat("History blend", &settings.taa_history_blend, 0.5f, 0.98f);
  }
  // Render scale (internal resolution). Only meaningful without an upscaler
  // driving the resolution; >1 supersamples and the post pass downscales to the
  // window. Committed on release so dragging does not resize every frame.
  if (settings.aa_mode != render::AntiAliasingMode::kUpscaler) {
    ImGui::SliderFloat("Render scale", &render_scale_ui_, 0.5f, 2.0f, "%.2fx");
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      settings.render_scale = render_scale_ui_;
    } else if (!ImGui::IsItemActive()) {
      render_scale_ui_ = settings.render_scale;  // mirror preset/external changes
    }
    if (settings.render_scale > 1.001f) {
      ImGui::SameLine();
      ImGui::TextDisabled("SSAA");
    }
  }
  ImGui::SeparatorText("Presentation");
  ImGui::Checkbox("VSync", &settings.vsync);
  // The renderer only requests an HDR swapchain when the OS is actually
  // compositing in HDR, so this is an intent toggle; the label shows why it
  // may stay SDR.
  ImGui::Checkbox("HDR output", &settings.hdr_output);
  if (window_) {
    ImGui::SameLine();
    ImGui::TextDisabled(window_->hdr_enabled() ? "(system hdr on)" : "(system hdr off: sdr)");
  }
}

void DebugUi::DrawRayTracingTab(render::Renderer& renderer, render::RenderSettings& settings,
                                const render::DeviceCaps* caps) {
  const bool ray_query = caps && caps->ray_query;
  if (!ray_query) {
    ImGui::TextColored({1, 0.6f, 0.3f, 1}, "no ray-query support; RT features disabled");
  }

  ImGui::SeparatorText("Shadows");
  ImGui::BeginDisabled(!ray_query);
  ImGui::Checkbox("Raytraced shadows", &settings.rt_shadows);
  if (settings.rt_shadows) {
    f32 degrees = settings.sun_angular_radius * 57.29578f;
    if (ImGui::SliderFloat("Sun radius (deg)", &degrees, 0.0f, 2.0f, "%.2f")) {
      settings.sun_angular_radius = degrees / 57.29578f;
    }
  }
  ImGui::EndDisabled();
  ImGui::Checkbox("Cascaded shadow maps", &settings.shadow_maps);
  if (settings.shadow_maps) {
    ImGui::SliderFloat("Shadow distance", &settings.shadow_distance, 30.0f, 400.0f, "%.0f");
    ImGui::TextDisabled(ray_query && settings.rt_shadows
                            ? "(rt shadows active; cascades are the fallback)"
                            : "raster sun shadows");
  }

  ImGui::SeparatorText("Reflections");
  ImGui::BeginDisabled(!ray_query);
  ImGui::Checkbox("Water RT reflections", &settings.water_reflections);
  ImGui::Checkbox("Adaptive water geometry", &settings.adaptive_water);
  int water_budget = static_cast<int>(settings.water_triangle_budget);
  if (ImGui::SliderInt("Water triangle budget", &water_budget, 1024, 32768))
    settings.water_triangle_budget = static_cast<u32>(water_budget);
  ImGui::SliderFloat("Water triangle pixels", &settings.water_target_triangle_pixels, 8.0f,
                     96.0f, "%.0f px");
  ImGui::Checkbox("Water foam field", &settings.water_field);
  ImGui::BeginDisabled(!settings.water_field);
  ImGui::Checkbox("  Local interaction (depth ripples + obstacles)",
                  &settings.water_interaction);
  ImGui::EndDisabled();
  ImGui::Checkbox("RT reflections", &settings.rt_reflections);
  if (settings.rt_reflections) {
    ImGui::SliderFloat("Reflection roughness", &settings.reflection_roughness_cutoff, 0.05f, 1.0f,
                       "%.2f");
  }
  ImGui::Checkbox("Screen-space reflections", &settings.ssr);
  ImGui::EndDisabled();

  ImGui::Checkbox("Shoreline wetting", &settings.shore_wetting);
  if (settings.shore_wetting) {
    ImGui::SliderFloat("Shore drying time", &settings.shore_drying_time, 5.0f, 60.0f, "%.0f s");
  }

  ImGui::SeparatorText("Water shading");
  ImGui::ColorEdit3("Water absorption (1/m)", settings.water_absorption,
                    ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
  ImGui::SliderFloat("Absorption scale", &settings.water_absorption_scale, 0.0f, 4.0f, "%.2f");
  ImGui::SliderFloat("Transmission", &settings.water_transmission, 0.0f, 1.5f, "%.2f");
  ImGui::SliderFloat("Reflection foam gain", &settings.water_refl_foam_gain, 0.0f, 2.0f, "%.2f");
  ImGui::SliderFloat("Crest SSS intensity", &settings.water_sss_intensity, 0.0f, 3.0f, "%.2f");
  ImGui::SliderFloat("Crest SSS exponent", &settings.water_sss_exponent, 1.0f, 12.0f, "%.1f");
  ImGui::Checkbox("Underwater caustics", &settings.water_caustics);
  if (settings.water_caustics) {
    ImGui::SliderFloat("Caustic intensity", &settings.water_caustic_intensity, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Caustic depth fade", &settings.water_caustic_depth_fade, 0.0f, 1.0f,
                       "%.2f /m");
    ImGui::SliderFloat("Caustic receiver depth", &settings.water_caustic_receiver_depth, 0.5f,
                       16.0f, "%.1f m");
    ImGui::SliderFloat("Water rest height", &settings.water_rest_height, -10.0f, 10.0f, "%.1f m");
  }

  ImGui::SeparatorText("Path tracing");
  ImGui::BeginDisabled(!ray_query);
  ImGui::Checkbox("Path tracing", &settings.path_trace);
  if (settings.path_trace) {
    ImGui::Checkbox("  Ground truth (no denoise)", &settings.path_trace_reference);
    if (!settings.path_trace_reference)
      ImGui::Checkbox("  Reconstruction renderer (SVGF)", &settings.path_trace_recon);
    if (settings.path_trace_reference) {
      ImGui::Text("  accumulated %u spp", renderer.path_trace_samples());
    } else if (settings.path_trace_recon) {
      const char* dbg[] = {"Final",  "Lighting", "History len", "Variance",
                           "Motion", "Normal",   "Albedo",      "Specular"};
      int d = static_cast<int>(settings.path_trace_recon_debug);
      if (ImGui::Combo("  Debug view", &d, dbg, IM_ARRAYSIZE(dbg)))
        settings.path_trace_recon_debug = static_cast<u32>(d);
      int ap = static_cast<int>(settings.path_trace_recon_atrous);
      if (ImGui::SliderInt("  A-trous passes", &ap, 0, 6))
        settings.path_trace_recon_atrous = static_cast<u32>(ap);
      ImGui::SliderFloat("  Responsiveness", &settings.path_trace_recon_weight, 0.01f, 0.5f);
      int spp = static_cast<int>(settings.path_trace_spp);
      if (ImGui::SliderInt("  Samples/pixel ", &spp, 1, 8))
        settings.path_trace_spp = static_cast<u32>(spp);
    } else {
      // More samples = lower input noise = less motion shimmer, at linear cost.
      int spp = static_cast<int>(settings.path_trace_spp);
      if (ImGui::SliderInt("  Samples/pixel", &spp, 1, 8))
        settings.path_trace_spp = static_cast<u32>(spp);
      // Lower accum = less ghosting/shadow lag but grainier (raise spp to compensate).
      int accum = static_cast<int>(settings.path_trace_accum);
      if (ImGui::SliderInt("  Denoiser history", &accum, 2, 48))
        settings.path_trace_accum = static_cast<u32>(accum);
    }
  }
  ImGui::EndDisabled();
}

void DebugUi::DrawLightingTab(render::RenderSettings& settings, const render::DeviceCaps* caps) {
  // Day/night cycle: scrub the time of day (drives the sun, sky and ambient) and
  // the rate game time passes. While the cycle runs it owns the sun, so the
  // manual sun controls below only stick when time is frozen (Time scale 0) or
  // RX_SUN_DIR pinned a fixed sun.
  if (clock_) {
    f32 hour = clock_->hour();
    if (ImGui::SliderFloat("Time of day", &hour, 0.0f, 24.0f, "%.2f h")) clock_->set_hour(hour);
    f32 timescale = clock_->timescale();
    if (ImGui::SliderFloat("Time scale", &timescale, 0.0f, 1200.0f, "%.0fx"))
      clock_->set_timescale(timescale);
  }
  ImGui::Checkbox("Procedural sky", &settings.sky);
  f32 direction[3] = {settings.sun_direction.x, settings.sun_direction.y, settings.sun_direction.z};
  if (ImGui::SliderFloat3("Sun direction", direction, -1.0f, 1.0f)) {
    settings.sun_direction = {direction[0], direction[1], direction[2]};
    if (settings.sun_direction.y > -0.05f) settings.sun_direction.y = -0.05f;
  }
  ImGui::SliderFloat("Sun intensity", &settings.sun_intensity, 0.0f, 20.0f);
  f32 color[3] = {settings.sun_color.x, settings.sun_color.y, settings.sun_color.z};
  if (ImGui::ColorEdit3("Sun color", color)) {
    settings.sun_color = {color[0], color[1], color[2]};
  }
  ImGui::SliderFloat("Ambient", &settings.ambient, 0.0f, 0.5f);

  ImGui::SeparatorText("Volumetric fog");
  ImGui::BeginDisabled(!caps || !caps->ray_query);
  ImGui::Checkbox("Volumetric fog", &settings.fog);
  if (settings.fog) {
    ImGui::SliderFloat("Fog density", &settings.fog_density, 0.0f, 0.2f, "%.3f");
    ImGui::SliderFloat("Fog height falloff", &settings.fog_height_falloff, 0.0f, 1.0f);
    ImGui::SliderFloat("Fog anisotropy", &settings.fog_anisotropy, 0.0f, 0.95f);
  }
  ImGui::EndDisabled();

}

void DebugUi::DrawGiTab(render::RenderSettings& settings, const render::DeviceCaps* caps) {
  const bool ray_query = caps && caps->ray_query;
  ImGui::Checkbox("Image based lighting", &settings.ibl);
  if (settings.ibl) {
    ImGui::SliderFloat("IBL intensity", &settings.ibl_intensity, 0.0f, 4.0f);
  }
  ImGui::BeginDisabled(!ray_query || !settings.ibl);
  ImGui::Checkbox("DDGI probes", &settings.ddgi);
  if (settings.ddgi) {
    ImGui::SliderFloat("Probe spacing", &settings.ddgi_spacing, 0.5f, 5.0f);
    ImGui::SliderFloat("GI intensity", &settings.ddgi_intensity, 0.0f, 4.0f);
  }
  ImGui::EndDisabled();

  ImGui::BeginDisabled(!ray_query || !settings.ibl);
  ImGui::Checkbox("RCGI (idTech8, experimental)", &settings.rcgi);
  if (settings.rcgi) {
    ImGui::SliderFloat("RCGI intensity", &settings.rcgi_intensity, 0.0f, 4.0f);
  }
  ImGui::EndDisabled();

  ImGui::SeparatorText("Ambient occlusion");
  ImGui::BeginDisabled(!ray_query);
  ImGui::Checkbox("RT ambient occlusion", &settings.rtao);
  ImGui::EndDisabled();
  ImGui::Checkbox("Screen-space AO (fallback)", &settings.ssao);
  if (settings.rtao || settings.ssao) {
    ImGui::SliderFloat("AO radius", &settings.ao_radius, 0.2f, 5.0f);
    ImGui::SliderFloat("AO intensity", &settings.ao_intensity, 0.2f, 3.0f);
    int rays = static_cast<int>(settings.ao_rays);
    if (ImGui::SliderInt("AO rays/taps", &rays, 1, 8)) settings.ao_rays = rays;
  }

  ImGui::SeparatorText("Screen-space GI");
  ImGui::BeginDisabled(!ray_query);
  ImGui::Checkbox("Screen-space GI", &settings.ssgi);
  ImGui::EndDisabled();
}

void DebugUi::DrawPostTab(render::RenderSettings& settings) {
  int tonemap = static_cast<int>(settings.tonemap);
  if (ImGui::Combo("Tonemap", &tonemap, kTonemaps, IM_ARRAYSIZE(kTonemaps))) {
    settings.tonemap = static_cast<render::TonemapOperator>(tonemap);
  }
  int grade = static_cast<int>(settings.color_grade);
  if (ImGui::Combo("Color grade", &grade, kColorGrades, IM_ARRAYSIZE(kColorGrades))) {
    settings.color_grade = static_cast<render::ColorGrade>(grade);
  }
  ImGui::Checkbox("Bloom", &settings.bloom);
  ImGui::Checkbox("Motion blur", &settings.motion_blur);
  if (settings.bloom) {
    ImGui::SliderFloat("Bloom intensity", &settings.bloom_intensity, 0.0f, 0.2f, "%.3f");
  }
  ImGui::Checkbox("Depth of field", &settings.dof);
  if (settings.dof) {
    ImGui::SliderFloat("Aperture", &settings.dof_aperture, 0.5f, 8.0f, "%.1f");
    ImGui::SliderFloat("Focus (m, 0 = auto)", &settings.dof_focus, 0.0f, 200.0f, "%.0f");
  }
  ImGui::SliderFloat("Chromatic aberration", &settings.chromatic_aberration, 0.0f, 4.0f, "%.1f px");
  ImGui::SliderFloat("Vignette", &settings.vignette, 0.0f, 1.0f, "%.2f");
  ImGui::SliderFloat("Film grain", &settings.film_grain, 0.0f, 0.06f, "%.3f");
  ImGui::SliderFloat("Lens flare", &settings.lens_flare, 0.0f, 0.3f, "%.2f");
  ImGui::Checkbox("Auto exposure", &settings.auto_exposure);
  if (settings.auto_exposure) {
    ImGui::SliderFloat("Adaptation speed", &settings.adaptation_speed, 0.5f, 10.0f);
  }
  ImGui::SliderFloat(settings.auto_exposure ? "Compensation" : "Exposure", &settings.exposure, 0.1f,
                     8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
}

void DebugUi::DrawDiagnosticsTab(render::Renderer& renderer, FlyCamera& camera,
                                 render::RenderSettings& settings, const render::DeviceCaps* caps) {
  int debug_view = static_cast<int>(settings.debug_view);
  if (ImGui::Combo("Debug view", &debug_view, kDebugViews, IM_ARRAYSIZE(kDebugViews))) {
    settings.debug_view = static_cast<render::DebugView>(debug_view);
  }
  ImGui::BeginDisabled(!caps || !caps->fill_mode_non_solid);
  ImGui::Checkbox("Wireframe", &settings.wireframe);
  ImGui::EndDisabled();
  ImGui::BeginDisabled(!caps || !caps->mesh_shaders);
  ImGui::Checkbox("Mesh-shader LOD path", &settings.mesh_shader_lod);
  ImGui::EndDisabled();
  if (caps && !caps->mesh_shaders) {
    ImGui::SameLine();
    ImGui::TextDisabled("(no VK_EXT_mesh_shader)");
  }

  const auto& timings = renderer.pass_timings();
  if (!timings.empty()) {
    if (ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::Text("gpu frame %.2f ms", renderer.gpu_frame_ms());
      if (ImGui::BeginTable("passes", 2,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        for (const auto& t : timings) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(t.name.c_str());
          ImGui::TableNextColumn();
          ImGui::Text("%.3f ms", t.ms);
        }
        ImGui::EndTable();
      }
    }
  }

  if (render::Device* device = renderer.device();
      device && ImGui::CollapsingHeader("GPU memory")) {
    render::Device::MemoryBudget mem = device->memory_budget();
    const f64 mb = 1.0 / (1024.0 * 1024.0);
    ImGui::Text("used %.0f / %.0f MB", mem.used_bytes * mb, mem.budget_bytes * mb);
    if (mem.budget_bytes > 0) {
      ImGui::ProgressBar(static_cast<f32>(static_cast<f64>(mem.used_bytes) / mem.budget_bytes),
                         {-1, 0});
    }
    ImGui::Text("%u allocations, %.0f MB live", mem.allocation_count, mem.allocated_bytes * mb);
    const render::RenderGraph::Stats& g = renderer.graph_stats();
    ImGui::Text("frame graph transients: %u (%.1f MB)", g.transient_count, g.transient_bytes * mb);
    ImGui::Text("opaque draws: %u / %u visible (gpu cull)", renderer.draws_visible(),
                renderer.draws_total());
    if (renderer.meshlets_total() > 0) {
      ImGui::Text("meshlets: %u / %u drawn (cluster cull)", renderer.meshlets_visible(),
                  renderer.meshlets_total());
    }
  }

  if (const render::RenderGraph::Stats& g = renderer.graph_stats();
      !g.passes.empty() && ImGui::CollapsingHeader("Frame graph")) {
    ImGui::Text("%zu passes, %u barriers", g.passes.size(), g.barrier_count);
    if (ImGui::BeginTable("fg_passes", 4,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingStretchProp,
                          {0, 180})) {
      ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("R", ImGuiTableColumnFlags_WidthFixed, 24);
      ImGui::TableSetupColumn("W", ImGuiTableColumnFlags_WidthFixed, 24);
      ImGui::TableSetupColumn("Bar", ImGuiTableColumnFlags_WidthFixed, 30);
      for (const auto& p : g.passes) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.name.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%u", p.reads);
        ImGui::TableNextColumn();
        ImGui::Text("%u", p.writes);
        ImGui::TableNextColumn();
        ImGui::Text("%u", p.barriers);
      }
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Physics")) {
    ImGui::TextDisabled("F throws a floating cube (jolt buoyancy)");
  }

  if (ImGui::CollapsingHeader("Camera")) {
    Vec3 position = camera.position();
    ImGui::Text("position %.1f %.1f %.1f", position.x, position.y, position.z);
    ImGui::SliderFloat("Speed", &camera.speed, 0.1f, 50.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::TextDisabled("RMB look, WASD move, Q/E down/up, shift fast");
  }

  if (ImGui::CollapsingHeader("Scene")) {
    ImGui::Text("meshes %u", renderer.mesh_count());
    if (const render::MaterialSystem* materials = renderer.materials()) {
      ImGui::Text("materials %u, textures %u", materials->material_count(),
                  materials->texture_count());
    }
    ImGui::Checkbox("ImGui demo window", &show_demo_);
  }
}

void DebugUi::DrawStatusBar(f32 frame_delta, const render::FrameView& view) {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  const ImVec2 bar_min = {viewport->WorkPos.x,
                          viewport->WorkPos.y + viewport->WorkSize.y - kStatusBarHeight};
  const ImVec2 bar_max = {viewport->WorkPos.x + viewport->WorkSize.x,
                          viewport->WorkPos.y + viewport->WorkSize.y};
  ImDrawList* dl = ImGui::GetForegroundDrawList(viewport);
  dl->AddRectFilled(bar_min, bar_max, IM_COL32(0, 0, 0, 255));
  dl->AddLine(bar_min, {bar_max.x, bar_min.y}, IM_COL32(255, 255, 255, 42));

  constexpr f32 kPadding = 12.0f;
  const bool compact = viewport->WorkSize.x < 900.0f;
  const bool two_rows = viewport->WorkSize.x < 600.0f;
  const f32 section_gap = compact ? 8.0f : 18.0f;
  const f32 graph_width = std::clamp(viewport->WorkSize.x * 0.22f, 72.0f, 280.0f);
  const ImVec2 graph_min = {bar_max.x - kPadding - graph_width, bar_min.y + 6.0f};
  const ImVec2 graph_max = {bar_max.x - kPadding, bar_max.y - 6.0f};

  char fps_text[48];
  const f32 fps = frame_delta > 0.0f ? 1.0f / frame_delta : 0.0f;
  if (compact)
    std::snprintf(fps_text, sizeof(fps_text), "%.0f FPS", fps);
  else
    std::snprintf(fps_text, sizeof(fps_text), "FPS %.0f  %.2f ms", fps,
                  frame_delta * 1000.0f);
  char location_text[80];
  if (compact)
    std::snprintf(location_text, sizeof(location_text), "XYZ %.1f %.1f %.1f", view.camera.eye.x,
                  view.camera.eye.y, view.camera.eye.z);
  else
    std::snprintf(location_text, sizeof(location_text), "XYZ %.2f  %.2f  %.2f",
                  view.camera.eye.x, view.camera.eye.y, view.camera.eye.z);
  char build_text[96];
  if (compact)
    std::snprintf(build_text, sizeof(build_text), "%s@%.8s", RX_VERSION, RX_BUILD_ID);
  else
    std::snprintf(build_text, sizeof(build_text), "BUILD %s (%s)", RX_VERSION, RX_BUILD_ID);
  char config_text[64];
  std::snprintf(config_text, sizeof(config_text), compact ? "%s/%s" : "%s / %s",
                RX_BUILD_CONFIG, kBuildMode);

  const f32 text_start = bar_min.x + kPadding;
  const f32 text_limit = graph_min.x - section_gap;
  auto draw_section = [&](f32* text_x, f32 text_y, const char* text) {
    const f32 width = ImGui::CalcTextSize(text).x;
    const bool has_previous = *text_x > text_start;
    const f32 draw_x = *text_x + (has_previous ? section_gap : 0.0f);
    if (draw_x + width > text_limit) return;
    if (has_previous) {
      const f32 separator_x = *text_x + section_gap * 0.5f;
      dl->AddLine({separator_x, text_y}, {separator_x, text_y + ImGui::GetFontSize()},
                  IM_COL32(255, 255, 255, 64));
    }
    dl->AddText({draw_x, text_y}, IM_COL32(255, 255, 255, 255), text);
    *text_x = draw_x + width;
  };

  if (two_rows) {
    char build_mode_text[128];
    if (viewport->WorkSize.x < 300.0f)
      std::snprintf(build_mode_text, sizeof(build_mode_text), "%.8s %s", RX_BUILD_ID, kBuildMode);
    else
      std::snprintf(build_mode_text, sizeof(build_mode_text), "%s %s", build_text, kBuildMode);
    f32 first_row_x = text_start;
    f32 second_row_x = text_start;
    draw_section(&first_row_x, bar_min.y + 5.0f, fps_text);
    draw_section(&first_row_x, bar_min.y + 5.0f, build_mode_text);
    draw_section(&second_row_x, bar_min.y + 22.0f, location_text);
    draw_section(&second_row_x, bar_min.y + 22.0f, RX_BUILD_CONFIG);
  } else {
    f32 text_x = text_start;
    const f32 text_y = bar_min.y + (kStatusBarHeight - ImGui::GetFontSize()) * 0.5f;
    draw_section(&text_x, text_y, fps_text);
    draw_section(&text_x, text_y, location_text);
    draw_section(&text_x, text_y, build_text);
    draw_section(&text_x, text_y, config_text);
  }

  // A fixed 0-120 FPS scale keeps changes visually comparable across frames.
  // Samples grow in from the right until the ring is full.
  dl->AddRect(graph_min, graph_max, IM_COL32(255, 255, 255, 52));
  const f32 graph_height = graph_max.y - graph_min.y;
  for (f32 guide_fps : {30.0f, 60.0f}) {
    const f32 y = graph_max.y - graph_height * (guide_fps / kStatusGraphMaxFps);
    dl->AddLine({graph_min.x, y}, {graph_max.x, y}, IM_COL32(255, 255, 255, 28));
  }

  if (frame_time_count_ > 1) {
    ImVec2 points[IM_ARRAYSIZE(frame_times_)];
    const u32 capacity = IM_ARRAYSIZE(frame_times_);
    const u32 oldest = (frame_time_cursor_ + capacity - frame_time_count_) % capacity;
    const f32 sample_step = graph_width / static_cast<f32>(capacity - 1);
    const f32 first_x = graph_max.x - sample_step * static_cast<f32>(frame_time_count_ - 1);
    for (u32 i = 0; i < frame_time_count_; ++i) {
      const f32 frame_ms = frame_times_[(oldest + i) % capacity];
      const f32 sample_fps = frame_ms > 0.0f ? 1000.0f / frame_ms : 0.0f;
      const f32 normalized = std::clamp(sample_fps / kStatusGraphMaxFps, 0.0f, 1.0f);
      points[i] = {first_x + sample_step * static_cast<f32>(i),
                   graph_max.y - graph_height * normalized};
    }
    dl->PushClipRect(graph_min, graph_max, true);
    dl->AddPolyline(points, static_cast<int>(frame_time_count_), IM_COL32(255, 255, 255, 255),
                    1.5f);
    dl->PopClipRect();
  }
}

void DebugUi::DrawStageChart(render::Renderer& renderer, f32 bottom_offset) {
  const auto& timings = renderer.pass_timings();
  if (timings.empty()) return;

  // Heaviest stages first; fold the long tail into a single "other" bar so the
  // chart stays legible.
  struct Bar {
    const char* name;
    f32 ms;
  };
  std::vector<Bar> bars;
  bars.reserve(timings.size());
  for (const auto& t : timings) bars.push_back({t.name.c_str(), t.ms});
  std::sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) { return a.ms > b.ms; });

  constexpr int kMaxBars = 6;
  f32 other_ms = 0.0f;
  if (static_cast<int>(bars.size()) > kMaxBars) {
    for (size_t i = kMaxBars; i < bars.size(); ++i) other_ms += bars[i].ms;
    bars.resize(kMaxBars);
  }
  if (other_ms > 0.0f) bars.push_back({"other", other_ms});
  if (bars.empty()) return;

  const f32 max_ms = bars.front().ms > 0.0f ? bars.front().ms : 1.0f;

  // Geometry, in screen space: this is a HUD pinned to the bottom-left, not a
  // movable window.
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const f32 margin = 16.0f;
  const f32 pad = 12.0f;
  const f32 row_h = 16.0f;
  const f32 row_gap = 5.0f;
  const f32 header_h = 24.0f;
  const f32 panel_w = 326.0f;
  const int rows = static_cast<int>(bars.size());
  const f32 panel_h = pad + header_h + rows * row_h + (rows - 1) * row_gap + pad;

  const ImVec2 p0 = {margin, screen.y - bottom_offset - margin - panel_h};
  const ImVec2 p1 = {p0.x + panel_w, screen.y - bottom_offset - margin};
  ImDrawList* dl = ImGui::GetForegroundDrawList();

  // Dark glass panel with a hairline border.
  dl->AddRectFilled(p0, p1, IM_COL32(14, 16, 22, 214), 9.0f);
  dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 26), 9.0f, 0, 1.0f);

  const f32 content_x = p0.x + pad;
  const f32 content_w = panel_w - 2.0f * pad;
  const f32 text_h = ImGui::GetTextLineHeight();

  // Header: title on the left, total GPU frame time on the right.
  ImFont* font = ImGui::GetFont();
  const f32 font_size = ImGui::GetFontSize();
  const f32 head_y = p0.y + pad;
  dl->AddText(font, font_size, {content_x, head_y}, IM_COL32(206, 214, 232, 255), "GPU STAGES");
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f ms", renderer.gpu_frame_ms());
  const ImVec2 total_sz = ImGui::CalcTextSize(buf);
  dl->AddText(font, font_size, {p1.x - pad - total_sz.x, head_y}, IM_COL32(170, 182, 205, 255),
              buf);

  auto lerp8 = [](int a, int b, f32 t) { return static_cast<int>(a + (b - a) * t + 0.5f); };

  f32 y = p0.y + pad + header_h;
  for (const Bar& bar : bars) {
    const f32 frac = bar.ms / max_ms;
    const f32 fclamp = frac < 0.0f ? 0.0f : (frac > 1.0f ? 1.0f : frac);

    // Track, then a cost-graded fill (teal when cheap, coral when it dominates).
    dl->AddRectFilled({content_x, y}, {content_x + content_w, y + row_h},
                      IM_COL32(255, 255, 255, 16), 3.5f);
    const f32 fill_w = content_w * fclamp;
    if (fill_w > 1.0f) {
      const ImU32 col = IM_COL32(lerp8(78, 240, fclamp), lerp8(201, 104, fclamp),
                                 lerp8(176, 82, fclamp), 235);
      dl->AddRectFilled({content_x, y}, {content_x + fill_w, y + row_h}, col, 3.5f);
    }

    const f32 ty = y + (row_h - text_h) * 0.5f;
    std::snprintf(buf, sizeof(buf), "%.2f", bar.ms);
    const ImVec2 vsz = ImGui::CalcTextSize(buf);
    const f32 val_x = content_x + content_w - 6.0f - vsz.x;
    // Clip the label so a long pass name never runs into the ms value.
    const ImVec4 clip = {content_x + 6.0f, y, val_x - 4.0f, y + row_h};
    dl->AddText(font, font_size, {content_x + 6.0f, ty}, IM_COL32(236, 240, 248, 255), bar.name,
                nullptr, 0.0f, &clip);
    dl->AddText(font, font_size, {val_x, ty}, IM_COL32(232, 237, 245, 255), buf);

    y += row_h + row_gap;
  }
}

void DebugUi::ScanPresetFiles() {
  preset_files_.clear();
  preset_files_scanned_ = true;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(PresetDir(), ec)) {
    if (entry.is_regular_file(ec) && entry.path().extension() == ".ini")
      preset_files_.push_back(entry.path().filename().string());
  }
  std::sort(preset_files_.begin(), preset_files_.end());
  if (preset_file_choice_ >= static_cast<int>(preset_files_.size())) preset_file_choice_ = 0;
}

}  // namespace rx

#else  // !RX_HAS_IMGUI

namespace rx {

DebugUi::DebugUi() = default;
DebugUi::~DebugUi() = default;
bool DebugUi::Initialize(Window&, render::Renderer&) { return false; }
void DebugUi::Shutdown() {}
void DebugUi::BeginFrame() {}
void DebugUi::Build(render::Renderer&, FlyCamera&, f32, render::FrameView*) {}
bool DebugUi::wants_mouse() const { return false; }
bool DebugUi::wants_keyboard() const { return false; }

}  // namespace rx

#endif  // RX_HAS_IMGUI
