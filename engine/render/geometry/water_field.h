#ifndef RX_RENDER_WATER_FIELD_H_
#define RX_RENDER_WATER_FIELD_H_

// Persistent, camera-following water-surface data field. Two nested rings
// (clipmap-style) of RGBA16F texels track ripple height/velocity and a foam
// density/age that ADVECTS with the dominant wave drift and DECAYS over
// seconds, so whitecaps streak and dissolve the way real foam does instead of
// flickering in place like the per-frame crest term. Ring 0 covers ~96 m around
// the camera at ~0.19 m/texel; ring 1 covers ~384 m. The water pixel shader
// samples the rings by world XZ through env slots 30/31 (+ a params CB in slot
// 32) when kFrameFlagWaterField is set. Objects push ripples/foam into the
// field through a bounded WaterDisturbance array (boat wakes, bobbing props).
//
// The rings are full-res resampled from the previous frame each step, snapping
// the origin to the texel grid: that makes any toroidal/scroll bookkeeping
// unnecessary and stops the field swimming under the camera.

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

// One object disturbance the field consumes this frame: a wake ripple and/or a
// foam splat at a world position, scaled by the object's motion. Laid out to
// match the GPU `Disturbance` struct (two float4s) so it memcpy's straight into
// the storage buffer.
struct WaterDisturbance {
  Vec3 position{};          // world (XZ used)
  f32 radius = 1.0f;        // meters
  f32 ripple_strength = 0;  // impulse added to the ripple velocity channel
  f32 foam_amount = 0;      // foam density injected
  f32 velocity_x = 0;       // object XZ velocity, biases the foam advection
  f32 velocity_z = 0;
};
static_assert(sizeof(WaterDisturbance) == 32);

class WaterField {
 public:
  static constexpr u32 kRingCount = 2;  // architecture extends to N trivially
  static constexpr u32 kSize = 512;     // texels per ring side
  // World half-extent (meters) each ring reaches from its origin. Ring 0:
  // 96 m across (~0.19 m/texel), ring 1: 384 m across (~0.75 m/texel).
  static constexpr f32 kRingHalfExtent[kRingCount] = {48.0f, 192.0f};

  struct UpdateParams {
    Vec3 camera_pos{};
    f32 time = 0;
    f32 dt = 0;
    u32 frame_slot = 0;
    bool fft_ocean = false;  // crest injection samples the FFT foam vs Gerstner
    const WaterDisturbance* disturbances = nullptr;
    u32 disturbance_count = 0;
    // Local interaction (depth-buffer ripple impulses + island obstacle
    // boundaries). Only meaningful on ring 0; a no-op unless `interaction`.
    bool interaction = false;
    bool obstacle = false;             // reflect ripples off the analytic island
    Mat4 view_proj{};                  // project each texel column to screen
    Mat4 inv_view_proj{};              // reconstruct geometry world pos from depth
    f32 island[4] = {0, 0, 10, 0};     // center xz, sigma, peak (obstacle terrain)
    f32 water_level = 0.0f;            // rest surface height the band tests against
    f32 render_size[2] = {0, 0};      // depth buffer resolution (for the depth Load)
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);
  bool available() const { return static_cast<bool>(pipeline_); }

  // Recenter+advect+decay, ripple wave step (ring 0), and injection, per ring.
  // `ocean_normal_foam` is the FFT normal/foam map (its .w channel), sampled for
  // crest injection when params.fft_ocean is set; pass {} for the Gerstner path.
  // `ocean_displacement` is the FFT ocean displacement map (its .y height);
  // interaction tests the waterline against the live swell when it is bound, and
  // an analytic proxy otherwise. `opaque_depth` is the frame's prepass depth
  // (always valid when the field is active — the caller must schedule this pass
  // after the prepass writes it); it is only read when params.interaction is set.
  void AddToGraph(RenderGraph& graph, const UpdateParams& params,
                  TextureView ocean_normal_foam, TextureView ocean_displacement,
                  ResourceHandle opaque_depth);

  // This frame's written texture for a ring, kept in GENERAL by the compute
  // chain (bind through EnvironmentSystem::InGeneral).
  TextureView ring_view(u32 ring) const { return rings_[ring][write_].view; }
  // Per-frame-slot uniform buffer holding the ring origins/extents the water
  // shader needs to map world XZ into each ring.
  const GpuBuffer& params_buffer(u32 frame_slot) const { return params_[frame_slot]; }

 private:
  // Matches the shader's WaterFieldParams CB (env slot 32).
  struct GpuParams {
    f32 ring[kRingCount][4];  // origin.xz, half_extent, texel_world
  };
  // Matches the shader's WaterDisturbance push array is unused; disturbances go
  // through the storage buffer keyed by GpuParams-independent layout above.
  static constexpr u32 kMaxDisturbances = 256;

  PipelineHandle pipeline_;
  SamplerHandle sampler_;               // linear clamp, for the resample
  GpuImage rings_[kRingCount][2];       // ping-pong per ring
  GpuImage mask_[2];                    // ring-0 waterline intersection band, ping-pong
  GpuBuffer params_[Device::kMaxFramesInFlight];
  GpuBuffer disturbances_[Device::kMaxFramesInFlight];
  f32 origin_[kRingCount][2] = {};      // snapped world origin xz, persisted
  u32 write_ = 0;                       // ping-pong index of this frame's target
  bool centered_ = false;               // first frame has no history
};

}  // namespace rx::render

#endif  // RX_RENDER_WATER_FIELD_H_
