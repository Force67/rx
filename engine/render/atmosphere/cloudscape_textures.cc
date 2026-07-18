#include "render/atmosphere/cloudscape_textures.h"

#include "core/log.h"
#include "shaders/cloudscape_base_noise_cs_hlsl.h"
#include "shaders/cloudscape_curl_cs_hlsl.h"
#include "shaders/cloudscape_detail_noise_cs_hlsl.h"
#include "shaders/cloudscape_weather_map_cs_hlsl.h"

namespace rx::render {
namespace {

// Mirrors WeatherPush in cloudscape_weather_map.cs.hlsl.
struct WeatherPush {
  u32 seed_a;
  f32 coverage_a;
  f32 cloud_type_a;
  f32 precip_a;
  u32 seed_b;
  f32 coverage_b;
  f32 cloud_type_b;
  f32 precip_b;
  f32 blend;
  u32 pad[3];
};

bool StateEqual(const CloudscapeMapState& a, const CloudscapeMapState& b) {
  return a.seed == b.seed && a.coverage == b.coverage && a.cloud_type == b.cloud_type &&
         a.precipitation == b.precipitation;
}

}  // namespace

bool CloudscapeTextures::Initialize(Device& device) {
  // Every bake writes exactly one storage image at slot 0; the three static
  // volumes take no inputs, the weather map rides its push constants.
  base_noise_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_cloudscape_base_noise_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}}}},
      .push_constant_size = 0,
      .debug_name = "cloudscape_base_noise",
  });
  detail_noise_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_cloudscape_detail_noise_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}}}},
      .push_constant_size = 0,
      .debug_name = "cloudscape_detail_noise",
  });
  curl_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_cloudscape_curl_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}}}},
      .push_constant_size = 0,
      .debug_name = "cloudscape_curl",
  });
  weather_pipeline_ = device.CreateComputePipeline({
      .shader = RX_SHADER(k_cloudscape_weather_map_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageImage}}}},
      .push_constant_size = sizeof(WeatherPush),
      .debug_name = "cloudscape_weather_map",
  });
  if (!base_noise_pipeline_ || !detail_noise_pipeline_ || !curl_pipeline_ || !weather_pipeline_) {
    RX_ERROR("cloudscape texture pipeline creation failed");
    Destroy(device);
    return false;
  }

  // Storage + sampled: the bakes write them, the raymarcher samples them.
  const TextureUsageFlags usage = kTextureUsageStorage | kTextureUsageSampled;
  base_noise_ =
      device.CreateImage3D(Format::kRGBA8Unorm, kBaseNoiseSize, kBaseNoiseSize, kBaseNoiseSize,
                           usage);
  detail_noise_ = device.CreateImage3D(Format::kRGBA8Unorm, kDetailNoiseSize, kDetailNoiseSize,
                                       kDetailNoiseSize, usage);
  curl_ = device.CreateImage2D(Format::kRG16Float, {kCurlSize, kCurlSize}, usage);
  weather_map_ = device.CreateImage2D(Format::kRGBA8Unorm, {kWeatherSize, kWeatherSize}, usage);
  if (!base_noise_ || !detail_noise_ || !curl_ || !weather_map_) {
    RX_WARN("cloudscape textures unavailable (no 3d image support)");
    Destroy(device);
    return false;
  }

  // Trilinear, wrap on all axes: the volumes and the weather map tile, so every
  // sample must repeat seamlessly across the wrap.
  sampler_ = device.GetSampler({.min_filter = Filter::kLinear,
                                .mag_filter = Filter::kLinear,
                                .mip_filter = Filter::kLinear,
                                .address_u = AddressMode::kRepeat,
                                .address_v = AddressMode::kRepeat,
                                .address_w = AddressMode::kRepeat});

  // Settle every image in GENERAL, where the bakes write and the raymarcher
  // reads them; the first-use transition from kUndefined happens here so the
  // graph passes never have to.
  device.ImmediateSubmit([this](CommandList& cmd) {
    TextureBarrier to_general[4] = {
        Transition(base_noise_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(detail_noise_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(curl_, ResourceState::kUndefined, ResourceState::kGeneral),
        Transition(weather_map_, ResourceState::kUndefined, ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  return true;
}

void CloudscapeTextures::Destroy(Device& device) {
  for (PipelineHandle* p : {&base_noise_pipeline_, &detail_noise_pipeline_, &curl_pipeline_,
                            &weather_pipeline_}) {
    if (*p) device.DestroyPipeline(*p);
    *p = {};
  }
  for (GpuImage* img : {&base_noise_, &detail_noise_, &curl_, &weather_map_}) {
    if (*img) device.DestroyImage(*img);
    *img = {};
  }
  noise_baked_ = false;
  weather_baked_ = false;
  last_map_blend_ = -1.0f;
}

bool CloudscapeTextures::ready() const {
  return base_noise_ && detail_noise_ && curl_ && weather_map_ && base_noise_pipeline_ &&
         detail_noise_pipeline_ && curl_pipeline_ && weather_pipeline_;
}

bool CloudscapeTextures::MapStateChanged(const CloudscapeControls& controls) const {
  return !StateEqual(controls.map_a, last_map_a_) || !StateEqual(controls.map_b, last_map_b_) ||
         controls.map_blend != last_map_blend_;
}

void CloudscapeTextures::AddToGraph(RenderGraph& graph, const CloudscapeControls& controls) {
  if (!ready()) return;

  if (!noise_baked_) {
    // The three static volumes hash on lattice coordinates alone (no inputs),
    // so they bake exactly once. They stay in GENERAL afterward; the write is
    // made visible to both the compute raymarcher and any graphics-stage
    // sampler with a memory barrier at the end of each bake.
    graph.AddPass(
        "cloudscape_base_noise", [](RenderGraph::PassBuilder&) {},
        [this](PassContext& ctx) {
          ctx.cmd->BindPipeline(base_noise_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::Storage(0, base_noise_)});
          ctx.cmd->Dispatch(kBaseNoiseSize / 4, kBaseNoiseSize / 4, kBaseNoiseSize / 4);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        });
    graph.AddPass(
        "cloudscape_detail_noise", [](RenderGraph::PassBuilder&) {},
        [this](PassContext& ctx) {
          ctx.cmd->BindPipeline(detail_noise_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::Storage(0, detail_noise_)});
          ctx.cmd->Dispatch(kDetailNoiseSize / 4, kDetailNoiseSize / 4, kDetailNoiseSize / 4);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        });
    graph.AddPass(
        "cloudscape_curl", [](RenderGraph::PassBuilder&) {},
        [this](PassContext& ctx) {
          ctx.cmd->BindPipeline(curl_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::Storage(0, curl_)});
          ctx.cmd->Dispatch2D({kCurlSize, kCurlSize});
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        });
    noise_baked_ = true;
  }

  // The caller quantizes the controls, so an exact compare is enough to gate the
  // regen: rebake only when a map-relevant field actually moved.
  if (!weather_baked_ || MapStateChanged(controls)) {
    WeatherPush push{};
    push.seed_a = controls.map_a.seed;
    push.coverage_a = controls.map_a.coverage;
    push.cloud_type_a = controls.map_a.cloud_type;
    push.precip_a = controls.map_a.precipitation;
    push.seed_b = controls.map_b.seed;
    push.coverage_b = controls.map_b.coverage;
    push.cloud_type_b = controls.map_b.cloud_type;
    push.precip_b = controls.map_b.precipitation;
    push.blend = controls.map_blend;
    graph.AddPass(
        "cloudscape_weather_map", [](RenderGraph::PassBuilder&) {},
        [this, push](PassContext& ctx) {
          ctx.cmd->BindPipeline(weather_pipeline_);
          ctx.cmd->BindTransient(0, {Bind::Storage(0, weather_map_)});
          ctx.cmd->Push(push);
          ctx.cmd->Dispatch2D({kWeatherSize, kWeatherSize});
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kComputeRead);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite, BarrierScope::kGraphicsRead);
        });
    last_map_a_ = controls.map_a;
    last_map_b_ = controls.map_b;
    last_map_blend_ = controls.map_blend;
    weather_baked_ = true;
  }
}

}  // namespace rx::render
