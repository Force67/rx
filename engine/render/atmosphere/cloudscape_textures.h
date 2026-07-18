#ifndef RX_RENDER_CLOUDSCAPE_TEXTURES_H_
#define RX_RENDER_CLOUDSCAPE_TEXTURES_H_

// The texture backbone for the volumetric cloudscape. Bakes three tileable
// procedural noise volumes/maps once on the GPU (a 3D base shape, a 3D erosion
// detail, a 2D curl flow field) and regenerates a 2D world-space weather map
// whenever the weather controls change. The raymarcher that consumes these owns
// none of them; it samples the views this exposes with the wrap sampler here.

#include "render/atmosphere/cloudscape_types.h"
#include "render/core/render_graph.h"
#include "render/rhi/device.h"

namespace rx::render {

class CloudscapeTextures {
 public:
  // Sizes are public so the raymarcher can derive its sampling constants
  // against the same numbers rather than hard-coding a second copy.
  static constexpr u32 kBaseNoiseSize = 128;    // 3D shape volume
  static constexpr u32 kDetailNoiseSize = 32;   // 3D erosion volume
  static constexpr u32 kCurlSize = 128;         // 2D curl field
  static constexpr u32 kWeatherSize = 512;      // 2D weather map
  static constexpr f32 kWeatherExtent = 60000.0f;  // world metres the map tiles over

  bool Initialize(Device& device);   // create images, pipelines, sampler
  void Destroy(Device& device);

  // Appends work to the frame graph: the three one-time noise bakes on the
  // first call, and a weather-map regeneration pass whenever the map-relevant
  // fields of `controls` differ from what was last baked.
  void AddToGraph(RenderGraph& graph, const CloudscapeControls& controls);

  TextureView base_noise_view() const { return base_noise_.view; }    // 128^3 RGBA8 3D
  TextureView detail_noise_view() const { return detail_noise_.view; }  // 32^3 RGBA8 3D
  TextureView curl_view() const { return curl_.view; }                // 128^2 RG16F 2D
  TextureView weather_map_view() const { return weather_map_.view; }  // 512^2 RGBA8 2D
  SamplerHandle sampler() const { return sampler_; }                  // trilinear, wrap
  f32 weather_map_extent() const { return kWeatherExtent; }
  bool ready() const;  // images + pipelines all created

 private:
  // True when the map-relevant fields of `controls` differ from the last bake.
  bool MapStateChanged(const CloudscapeControls& controls) const;

  PipelineHandle base_noise_pipeline_;
  PipelineHandle detail_noise_pipeline_;
  PipelineHandle curl_pipeline_;
  PipelineHandle weather_pipeline_;

  // Persistent storage images: bake passes transition them to kGeneral for the
  // write and back to sampled-compute state for every consumer.
  GpuImage base_noise_;
  GpuImage detail_noise_;
  GpuImage curl_;
  GpuImage weather_map_;
  SamplerHandle sampler_;

  bool noise_baked_ = false;    // the three static volumes bake exactly once
  bool weather_baked_ = false;  // the weather map bakes once, then on change
  CloudscapeMapState last_map_a_{};
  CloudscapeMapState last_map_b_{};
  f32 last_map_blend_ = -1.0f;  // sentinel: never matches a real blend
};

}  // namespace rx::render

#endif  // RX_RENDER_CLOUDSCAPE_TEXTURES_H_
