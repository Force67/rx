#ifndef RX_RENDER_SURFACE_WEATHER_H_
#define RX_RENDER_SURFACE_WEATHER_H_

#include "core/math.h"
#include "render/core/render_graph.h"
#include "render/rhi/resources.h"

namespace rx::render {

class Device;

// Surface weather: applies precipitation's effect on the world to the lit scene
// (rain wetness = darken + sky-reflection sheen on up-faces; snow = white
// accumulation on up-faces), using the G-buffer normals + depth and the sky
// cubemap for the puddle reflection. Wetness and snow cover are independent
// channels, so melting snow and wet ground coexist; the sky-occlusion map keeps
// both from accumulating under bridges and roofs (soft dilated drip line).
// Cheap fullscreen pass; runs when either channel > 0.
class SurfaceWeather {
 public:
  struct Frame {
    Mat4 inv_view_proj;
    Vec3 camera_pos;
    f32 wetness = 0.0f;     // 0 dry .. 1 soaked
    f32 snow_cover = 0.0f;  // 0 bare .. 1 blanketed
    f32 rain = 0.0f;        // live rain 0..1; drives ripple rings, not wetness
    f32 time = 0.0f;        // seconds, animates the puddle ripples
    // Sky-occlusion map (PrecipOcclusion); occl_range <= 0 disables the gating
    // (everything counts as sky-visible).
    f32 occl[4] = {0, 0, 0, 0};
    f32 occl_range = 0.0f;
    TextureView occlusion;
    SamplerHandle occlusion_sampler;
  };

  bool Initialize(Device& device);
  void Destroy(Device& device);

  // `normals`/`depth` are the G-buffer; `sky_view`/`sky_sampler` the sky cubemap.
  ResourceHandle AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle normals,
                            ResourceHandle depth, TextureView sky_view, SamplerHandle sky_sampler,
                            Extent2D extent, const Frame& frame);

 private:
  PipelineHandle pipeline_;
};

}  // namespace rx::render

#endif  // RX_RENDER_SURFACE_WEATHER_H_
