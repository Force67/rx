#include "render/atmosphere/cloudscape.h"

#include <cmath>

#include "core/log.h"
#include "render/rhi/device.h"
#include "shaders/cloudscape_march_cs_hlsl.h"
#include "shaders/cloudscape_apply_cs_hlsl.h"

namespace rx::render {
namespace {

struct MarchPush {
  Mat4 inv_view_proj;
  Mat4 prev_view_proj;
  f32 camera_pos[4];     // xyz eye, w time
  f32 sun_direction[4];  // xyz travel dir, w intensity
  f32 sun_color[4];      // rgb, w ambient scale
  f32 wind[4];           // xy blow dir, z speed, w vertical skew
  f32 shape[4];          // bottom, top, density, turbulence
  f32 map[4];            // xy offset, z extent, w anvil
  u32 size[2];
  u32 full_size[2];
  u32 frame_index;
  u32 steps;
  u32 flags;
  f32 pad;
};
static_assert(sizeof(MarchPush) <= 256, "march push must fit the 256B budget");

struct ApplyPush {
  u32 full_size[2];
  u32 half_size[2];
  f32 flash;
  f32 pad;
};

}  // namespace

bool Cloudscape::Initialize(Device& device) {
  device_ = &device;
  if (!textures_.Initialize(device)) return false;
  march_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_cloudscape_march_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kStorageImage},
                          {2, BindingType::kSampledImage},
                          {3, BindingType::kSampledImage},
                          {4, BindingType::kSampledImage},
                          {5, BindingType::kCombinedTextureSampler},
                          {6, BindingType::kCombinedTextureSampler},
                          {7, BindingType::kCombinedTextureSampler},
                          {8, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(MarchPush),
      .debug_name = "cloudscape_march",
  });
  apply_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_cloudscape_apply_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kCombinedTextureSampler}}}},
      .push_constant_size = sizeof(ApplyPush),
      .debug_name = "cloudscape_apply",
  });
  if (!march_pipeline_ || !apply_pipeline_) {
    RX_ERROR("cloudscape pipeline creation failed");
    return false;
  }
  return true;
}

void Cloudscape::Destroy(Device& device) {
  ReleaseBuffers(device);
  device.DestroyPipeline(march_pipeline_);
  device.DestroyPipeline(apply_pipeline_);
  march_pipeline_ = {};
  apply_pipeline_ = {};
  textures_.Destroy(device);
}

void Cloudscape::ReleaseBuffers(Device& device) {
  for (int i = 0; i < 2; ++i) {
    if (cloud_[i]) device.DestroyImageDeferred(cloud_[i]);
    if (dist_[i]) device.DestroyImageDeferred(dist_[i]);
    cloud_[i] = {};
    dist_[i] = {};
    cloud_state_[i] = ResourceState::kUndefined;
    dist_state_[i] = ResourceState::kUndefined;
  }
  half_extent_ = {};
  history_valid_ = false;
}

void Cloudscape::EnsureBuffers(Device& device, Extent2D half) {
  if (half.width == half_extent_.width && half.height == half_extent_.height && cloud_[0]) return;
  ReleaseBuffers(device);
  TextureUsageFlags usage = kTextureUsageSampled | kTextureUsageStorage;
  for (int i = 0; i < 2; ++i) {
    cloud_[i] = device.CreateImage2D(Format::kRGBA16Float, half, usage);
    dist_[i] = device.CreateImage2D(Format::kR32Float, half, usage);
  }
  if (!cloud_[0] || !cloud_[1] || !dist_[0] || !dist_[1]) {
    RX_ERROR("cloudscape history buffer creation failed");
    ReleaseBuffers(device);
    return;
  }
  half_extent_ = half;
}

ResourceHandle Cloudscape::AddToGraph(RenderGraph& graph, ResourceHandle color,
                                      ResourceHandle depth, Extent2D extent, const Frame& frame) {
  if (!device_) return color;
  Device* device = device_;
  textures_.AddToGraph(graph, frame.controls);
  if (!textures_.ready()) return color;

  Extent2D half{extent.width > 1 ? extent.width / 2 : 1, extent.height > 1 ? extent.height / 2 : 1};
  EnsureBuffers(*device, half);
  if (!cloud_[0]) return color;

  u32 cur = slot_;
  u32 prv = slot_ ^ 1u;
  ResourceHandle cur_cloud = graph.ImportImage("cloudscape_cur", cloud_[cur], &cloud_state_[cur]);
  ResourceHandle cur_dist = graph.ImportImage("cloudscape_cur_d", dist_[cur], &dist_state_[cur]);
  ResourceHandle prv_cloud = graph.ImportImage("cloudscape_prv", cloud_[prv], &cloud_state_[prv]);
  ResourceHandle prv_dist = graph.ImportImage("cloudscape_prv_d", dist_[prv], &dist_state_[prv]);

  bool history = history_valid_;
  graph.AddPass(
      "cloudscape_march",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(cur_cloud, ResourceUsage::kStorageWrite);
        b.Write(cur_dist, ResourceUsage::kStorageWrite);
        b.Read(prv_cloud, ResourceUsage::kSampledCompute);
        b.Read(prv_dist, ResourceUsage::kSampledCompute);
        b.Read(depth, ResourceUsage::kSampledCompute);
        // The noise/weather textures live permanently in kGeneral with their
        // own barriers (CloudscapeTextures), so they are not declared here.
      },
      [this, cur_cloud, cur_dist, prv_cloud, prv_dist, depth, half, extent, frame,
       history](PassContext& ctx) {
        MarchPush push{};
        push.inv_view_proj = frame.inv_view_proj;
        push.prev_view_proj = frame.prev_view_proj;
        push.camera_pos[0] = frame.camera_pos.x;
        push.camera_pos[1] = frame.camera_pos.y;
        push.camera_pos[2] = frame.camera_pos.z;
        push.camera_pos[3] = frame.time;
        Vec3 sun = Normalize(frame.sun_direction);
        push.sun_direction[0] = sun.x;
        push.sun_direction[1] = sun.y;
        push.sun_direction[2] = sun.z;
        push.sun_direction[3] = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.sun_color[3] = frame.ambient;
        const CloudscapeControls& c = frame.controls;
        push.wind[0] = std::cos(c.wind_yaw);
        push.wind[1] = std::sin(c.wind_yaw);
        push.wind[2] = c.wind_speed;
        push.wind[3] = c.vertical_skew;
        push.shape[0] = c.bottom;
        push.shape[1] = c.top;
        push.shape[2] = c.density;
        push.shape[3] = c.turbulence;
        push.map[0] = c.map_offset.x;
        push.map[1] = c.map_offset.y;
        push.map[2] = textures_.weather_map_extent();
        push.map[3] = c.anvil;
        push.size[0] = half.width;
        push.size[1] = half.height;
        push.full_size[0] = extent.width;
        push.full_size[1] = extent.height;
        push.frame_index = frame.frame_index;
        push.steps = frame.steps;
        push.flags = history ? 1u : 0u;

        ctx.cmd->BindPipeline(march_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(cur_cloud)),
                Bind::Storage(1, ctx.graph->image(cur_dist)),
                Bind::Sampled(2, ctx.graph->image(prv_cloud)),
                Bind::Sampled(3, ctx.graph->image(prv_dist)),
                Bind::Sampled(4, ctx.graph->image(depth)),
                InGeneral(Bind::Combined(5, textures_.base_noise_view(), textures_.sampler())),
                InGeneral(Bind::Combined(6, textures_.detail_noise_view(), textures_.sampler())),
                InGeneral(Bind::Combined(7, textures_.curl_view(), textures_.sampler())),
                InGeneral(Bind::Combined(8, textures_.weather_map_view(), textures_.sampler()))});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(half);
      });

  ResourceHandle out = graph.CreateTexture({.name = "cloudscape",
                                            .format = Format::kRGBA16Float,
                                            .width = extent.width,
                                            .height = extent.height});
  graph.AddPass(
      "cloudscape_apply",
      [&](RenderGraph::PassBuilder& b) {
        b.Write(out, ResourceUsage::kStorageWrite);
        b.Read(color, ResourceUsage::kSampledCompute);
        b.Read(cur_cloud, ResourceUsage::kSampledCompute);
      },
      [this, out, color, cur_cloud, extent, half, flash = frame.flash](PassContext& ctx) {
        ApplyPush push{};
        push.full_size[0] = extent.width;
        push.full_size[1] = extent.height;
        push.half_size[0] = half.width;
        push.half_size[1] = half.height;
        push.flash = flash;
        ctx.cmd->BindPipeline(apply_pipeline_);
        ctx.cmd->BindTransient(
            0, {Bind::Storage(0, ctx.graph->image(out)),
                Bind::Sampled(1, ctx.graph->image(color)),
                Bind::Combined(2, ctx.graph->image(cur_cloud).view, textures_.sampler())});
        ctx.cmd->Push(push);
        ctx.cmd->Dispatch2D(extent);
      });

  slot_ ^= 1u;
  history_valid_ = true;
  return out;
}

}  // namespace rx::render
