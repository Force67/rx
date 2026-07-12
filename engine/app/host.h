#ifndef RX_ENGINE_APP_HOST_H_
#define RX_ENGINE_APP_HOST_H_

#include <atomic>
#include <memory>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>

#include "app/application.h"
#include "app/services.h"
#include "core/export.h"
#include "core/frame_timer.h"

namespace rx::app {

// The composition root: owns every engine subsystem, brings them up in order,
// runs the fixed-step simulation + render loop and tears them down. Game
// policy enters only through the Application callbacks; the host appears in
// no engine signature below this layer.
class RX_APP_EXPORT Host {
 public:
  Host() = default;
  ~Host();

  Host(const Host&) = delete;
  Host& operator=(const Host&) = delete;

  // `window` lets a platform supply its own surface; when null the host
  // creates one itself. A failed Initialize tears down whatever it had
  // brought up (the destructor calls Shutdown), so callers need not Shutdown
  // after a failure. `app` must outlive the host.
  bool Initialize(const AppConfig& config, Application& app,
                  std::unique_ptr<Window> window = nullptr);
  int Run();
  // One iteration of the main loop. Returns false when the host wants to
  // stop. Platforms that own the loop drive this directly instead of the
  // blocking Run().
  bool RunFrame();
  void Shutdown();

  // Surface lifecycle for platforms whose window can go away (mobile).
  void OnSurfaceDestroyed();
  void OnSurfaceCreated();

  // Safe to call from a signal handler; Run() returns after the current frame.
  void RequestQuit() { quit_.store(true, std::memory_order_relaxed); }

  Services& services() { return services_; }

  // (Re)seeds the day/night clock. `base_timescale` is the caller's authored
  // day-length (game-clock seconds per real second); RX_TIMESCALE / RX_GAME_HOUR
  // override the timescale and start hour. The host seeds it once at startup; an
  // application calls this again once content supplies its own authored
  // timescale.
  void ConfigureClock(f32 base_timescale);

 private:
  // Resolves the configured quality tier from the gpu (or a forced preset)
  // and applies it to the renderer's live settings, carrying the RX_* debug
  // env overrides through.
  void ApplyRenderPreset();
  // Fills view.draws from every visible Transform+Renderable entity, keeping
  // last frame's world matrices for motion vectors.
  void GatherEntityDraws(render::FrameView& view);

  AppConfig config_;
  Application* app_ = nullptr;

  std::unique_ptr<Window> window_;
  std::unique_ptr<JobSystem> jobs_;
  FrameTimer timer_;
  // The clock driving the day/night cycle; applications derive sun/sky from
  // it (or ignore it entirely).
  WorldClock clock_;

  ecs::World world_;
  ecs::Scheduler scheduler_;

  asset::Vfs vfs_;
  std::unique_ptr<audio::AudioSystem> audio_;

  render::Renderer renderer_;
  physics::PhysicsWorld physics_;
  base::Vector<PhysicsBinding> physics_bindings_;
  base::Vector<HairStrandBinding> hair_bindings_;
  base::Vector<f32> hair_positions_;  // strand readback scratch

  // Device-agnostic input: bindings + the per-frame resolved action snapshot.
  InputMap input_map_;
  ActionState actions_;

  // Last frame's world matrices keyed by entity, for motion vectors.
  base::UnorderedMap<u64, Mat4> prev_transforms_;

  Services services_;

  std::atomic<bool> quit_ = false;
  bool shut_down_ = false;
};

}  // namespace rx::app

#endif  // RX_ENGINE_APP_HOST_H_
