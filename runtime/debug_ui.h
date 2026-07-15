#ifndef RX_RUNTIME_DEBUG_UI_H_
#define RX_RUNTIME_DEBUG_UI_H_

// The overlay records through the engine's RHI imgui render backend
// (render/util/imgui_renderer.h) - no raw Vulkan, no volk here.
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/types.h"
#include "core/window.h"
#include "core/world_clock.h"
#include "render/core/renderer.h"

#if defined(RX_HAS_IMGUI)
#include "render/util/imgui_renderer.h"
#endif

namespace rx {

class FlyCamera;
namespace ecs {
class World;
}

// Dear ImGui overlay: frame stats plus live toggles for every render
// feature. Rendered through the renderer's ui pass straight onto the
// backbuffer. Compiles to a stub when imgui/SDL3 are unavailable.
class DebugUi {
 public:
  DebugUi();
  ~DebugUi();

  DebugUi(const DebugUi&) = delete;
  DebugUi& operator=(const DebugUi&) = delete;

  bool Initialize(Window& window, render::Renderer& renderer);
  // Call between renderer WaitIdle and renderer Shutdown.
  void Shutdown();

  // Starts the imgui frame; call once per frame after PumpEvents.
  void BeginFrame();
  // Builds the panels and fills view->ui_draw. Always pairs with a
  // BeginFrame, even while hidden.
  void Build(render::Renderer& renderer, FlyCamera& camera, const ecs::World& world,
             f32 frame_delta, render::FrameView* view);

  // The day/night clock, so the Lighting panel can scrub the time of day and the
  // timescale. Null leaves those controls out.
  void set_clock(WorldClock* clock) { clock_ = clock; }
  void ToggleVisible() { visible_ = !visible_; }
  void SetVisible(bool v) { visible_ = v; }
  // Hide/show every overlay window at once, for clean captures.
  void SetAllVisible(bool v) { visible_ = v; }
  bool wants_mouse() const;
  bool wants_keyboard() const;

 private:
  // Per-topic bodies of the render-settings window's tab bar. Each draws one
  // area of the pipeline; Build() wraps them in tab items so the long flat list
  // of options is split into navigable submenus.
  void DrawDisplayTab(render::Renderer& renderer, render::RenderSettings& settings);
  void DrawRayTracingTab(render::Renderer& renderer, render::RenderSettings& settings,
                         const render::DeviceCaps* caps);
  void DrawLightingTab(render::RenderSettings& settings, const render::DeviceCaps* caps);
  void DrawGiTab(render::RenderSettings& settings, const render::DeviceCaps* caps);
  void DrawPostTab(render::RenderSettings& settings);
  void DrawDiagnosticsTab(render::Renderer& renderer, FlyCamera& camera,
                          render::RenderSettings& settings, const render::DeviceCaps* caps);

  // Refreshes preset_files_ from the .ini files in the presets directory.
  void ScanPresetFiles();

  // Window-less GPU-stage bar chart pinned to the bottom-left, drawn straight
  // onto the foreground draw list from the renderer's per-pass timings.
  void DrawStageChart(render::Renderer& renderer, f32 bottom_offset);

  // Passive full-width build/performance strip pinned to the viewport bottom.
  void DrawStatusBar(render::Renderer& renderer, const ecs::World& world,
                     f32 frame_delta, const render::FrameView& view);

  bool initialized_ = false;
  bool visible_ = true;
  bool status_bar_visible_ = true;
  Window* window_ = nullptr;  // for the live system-HDR state in the Display tab
  // Per-pass GPU timestamps follow overlay visibility; the boot value
  // (RX_GPU_TIMINGS / preset) is latched so a forced run keeps them.
  bool gpu_timings_latched_ = false;
  bool gpu_timings_forced_ = false;
  bool show_demo_ = false;
  WorldClock* clock_ = nullptr;  // day/night cycle, for the Lighting time controls
  int preset_choice_ = 0;  // 0 = custom/hand-tuned, else a QualityPreset combo row
  // Editable .ini render presets (engine/render/presets): the discovered file
  // list (lazy-scanned, rescannable), the combo selection, the save-as name
  // buffer and the last load/save status line.
  std::vector<std::string> preset_files_;
  bool preset_files_scanned_ = false;
  int preset_file_choice_ = 0;
  char preset_save_name_[64] = "custom";
  std::string preset_status_;
#if defined(RX_HAS_IMGUI)
  render::ImGuiRenderer imgui_renderer_;  // RHI imgui render backend
#endif
  f32 render_scale_ui_ = 1.0f;  // in-progress render-scale slider; committed on release
  f32 frame_times_[150] = {};
  u32 frame_time_cursor_ = 0;
  u32 frame_time_count_ = 0;
  f64 next_memory_sample_time_ = 0;
  u64 cpu_memory_bytes_ = 0;
  u64 cpu_memory_limit_bytes_ = 0;
  u64 gpu_memory_bytes_ = 0;
  u64 gpu_memory_budget_bytes_ = 0;
};

}  // namespace rx

#endif  // RX_RUNTIME_DEBUG_UI_H_
