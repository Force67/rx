#ifndef RX_RENDER_LIGHTNING_H_
#define RX_RENDER_LIGHTNING_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class Device;
struct PointLight;
struct WeatherSettings;

// Lightning strikes. The game schedules a strike (WeatherSettings::strike_*);
// rx renders everything about it: a procedural branched bolt whose segments
// are a pure function of (strike_seed, strike_pos, SV_InstanceID) - no CPU
// geometry, matching the precip volume's stateless philosophy - plus one
// positioned flash light appended to the frame's light list, so the froxel
// volumetrics, clustered surfaces, particles and (via the clustered path) RT
// reflections all bloom from the bolt's position. The global
// `weather.lightning` scalar keeps its existing sun/ambient/cloud boosts; the
// bolt and the flash light add locality on top.
class LightningSystem {
 public:
  // A strike renders for this long: an instant main stroke plus up to two
  // deterministic re-strokes (the classic flicker) inside the window.
  static constexpr f32 kStrikeDuration = 0.45f;

  struct Frame {
    Mat4 view_proj;
    Vec3 cam_pos;
    f32 time = 0.0f;
    Vec3 strike_pos;         // world, ground end of the channel
    f32 strike_age = -1.0f;  // seconds since the strike began
    u32 strike_seed = 0;
    f32 strike_energy = 1.0f;
    f32 jitter[2] = {0, 0};
  };

  // Deterministic stroke envelope, mirrored in lightning_bolt shaders: instant
  // attack decaying over ~80 ms, then 1-2 re-strokes within the window whose
  // timing/amplitude hash off the seed. Also used by the flash light and by
  // demo/game-side schedulers that want the global flash to agree with the bolt.
  static f32 Envelope(f32 age, u32 seed);

  bool Initialize(Device& device, Format color_format);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_); }

  // Appends the strike's positioned flash light (one point light at the
  // channel's mid height) when a strike is active. Called right after the
  // frame's view-lights copy so the light participates in local-shadow
  // assignment, clustering and the froxel volumetrics like any other light.
  u32 AppendLights(PointLight* dst, u32 remaining_capacity,
                   const WeatherSettings& weather) const;

  // Draws the bolt over the lit scene: color is the lit target, depth the
  // prepass depth export (mountains occlude distant bolts), motion the
  // velocity target (the solid core writes zero motion so TAA does not drag
  // sky velocity through the flash).
  void AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                  ResourceHandle motion, const Frame& frame);

 private:
  PipelineHandle pipeline_;
};

}  // namespace rx::render

#endif  // RX_RENDER_LIGHTNING_H_
