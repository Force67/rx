#include "render/atmosphere/precip_volume.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/gi/raytracing.h"
#include "shaders/precip_splash_ps_hlsl.h"
#include "shaders/precip_splash_vs_hlsl.h"
#include "shaders/precip_volume_ps_hlsl.h"
#include "shaders/precip_volume_rt_vs_hlsl.h"
#include "shaders/precip_volume_vs_hlsl.h"

namespace rx::render {
namespace {

constexpr Format kPrecipMotionFormat = Format::kRG16Float;  // == kMotionFormat

// Mirrors PrecipPush in precip_common.hlsli. 256 bytes, exactly the guaranteed
// push cap on the desktop targets - do not grow it.
struct PrecipPush {
  Mat4 view_proj;
  Mat4 prev_view_proj;
  f32 cam_right[3]; f32 time;
  f32 cam_up[3]; f32 intensity;
  f32 cam_pos[3]; u32 flags;  // 1 snow, 2 froxel volume valid
  f32 sun_dir[3]; f32 sun_intensity;
  f32 sun_color[3]; f32 ambient;
  f32 wind[4];  // xy wind velocity xz, z gustiness, w lightning
  f32 occl[4];
  f32 jitter[2]; f32 dt; f32 occl_range;
};
static_assert(sizeof(PrecipPush) == 256);

}  // namespace

bool PrecipVolume::Initialize(Device& device, Format color_format, bool ray_query) {
  // Slot 0 is the sky-occlusion map (vertex stage), 1 the prepass depth and 2
  // the froxel volume (pixel stage); the rt variant adds the TLAS at 3.
  base::Vector<PipelineBindings> sets;
  sets.push_back({.slots = {{0, BindingType::kCombinedTextureSampler},
                            {1, BindingType::kSampledImage},
                            {2, BindingType::kCombinedTextureSampler}}});

  // Attachment 0 = lit colour, 1 = motion, both alpha-weighted like the
  // billboard particles. Depth is read as a texture (soft fade), not attached.
  GraphicsPipelineDesc desc{
      .vertex = RX_SHADER(k_precip_volume_vs_hlsl),
      .fragment = RX_SHADER(k_precip_volume_ps_hlsl),
      .topology = PrimitiveTopology::kTriangleStrip,
      .raster = {.cull = CullMode::kNone},
      .color_formats = {color_format, kPrecipMotionFormat},
      .blend = {BlendMode::kAlpha, BlendMode::kAlpha},
      .sets = sets,
      .push_constant_size = sizeof(PrecipPush),
      .debug_name = "precip_volume",
  };
  pipeline_ = device.CreateGraphicsPipeline(desc);
  if (!pipeline_) {
    RX_ERROR("precip volume pipeline creation failed");
    return false;
  }

  if (ray_query) {
    base::Vector<PipelineBindings> rt_sets;
    rt_sets.push_back({.slots = {{0, BindingType::kCombinedTextureSampler},
                                 {1, BindingType::kSampledImage},
                                 {2, BindingType::kCombinedTextureSampler},
                                 {3, BindingType::kAccelStruct}}});
    desc.vertex = RX_SHADER(k_precip_volume_rt_vs_hlsl);
    desc.sets = rt_sets;
    desc.debug_name = "precip_volume_rt";
    pipeline_rt_ = device.CreateGraphicsPipeline(desc);
    if (!pipeline_rt_) {
      RX_WARN("precip volume rt variant unavailable; drops stay unshadowed");
    }
  }

  desc.vertex = RX_SHADER(k_precip_splash_vs_hlsl);
  desc.fragment = RX_SHADER(k_precip_splash_ps_hlsl);
  desc.sets = sets;
  desc.debug_name = "precip_splash";
  splash_pipeline_ = device.CreateGraphicsPipeline(desc);
  if (!splash_pipeline_) {
    RX_ERROR("precip splash pipeline creation failed");
    return false;
  }

  // Froxel fog may never come up (its Initialize failure is nonfatal), yet
  // slot 2 must always hold a valid Texture3D descriptor. A cleared 1x1x1
  // stand-in fills it then; the froxel flag in the push zeroes the term.
  froxel_dummy_ = device.CreateImage3D(
      Format::kRGBA16Float, 1, 1, 1,
      kTextureUsageStorage | kTextureUsageSampled | kTextureUsageTransferDst);
  if (!froxel_dummy_) {
    RX_ERROR("precip froxel stand-in creation failed");
    return false;
  }
  device.ImmediateSubmit([this](CommandList& cmd) {
    TextureBarrier to_clear[1] = {
        Transition(froxel_dummy_, ResourceState::kUndefined, ResourceState::kCopyDst)};
    cmd.TextureBarriers(to_clear);
    const f32 zero[4] = {0, 0, 0, 0};
    cmd.ClearColor(froxel_dummy_, zero);
    TextureBarrier to_general[1] = {
        Transition(froxel_dummy_, ResourceState::kCopyDst, ResourceState::kGeneral)};
    cmd.TextureBarriers(to_general);
  });
  return true;
}

void PrecipVolume::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
  device.DestroyPipeline(pipeline_rt_);
  pipeline_rt_ = {};
  device.DestroyPipeline(splash_pipeline_);
  splash_pipeline_ = {};
  device.DestroyImage(froxel_dummy_);
  froxel_dummy_ = {};
}

void PrecipVolume::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                              ResourceHandle motion, RayTracingContext* raytracing, u32 tlas_slot,
                              const Frame& frame) {
  if (!available()) return;
  f32 intensity = std::clamp(frame.intensity, 0.0f, 1.0f);
  u32 max_drops = frame.snow ? kMaxSnow : kMaxRain;
  u32 drop_count = static_cast<u32>(static_cast<f32>(max_drops) * intensity);
  // Splash cells thin by intensity in the shader; keep the instance count flat
  // so light rain still lands the occasional drop.
  u32 splash_count = (!frame.snow && drop_count > 0) ? kMaxSplashes * 2 : 0;
  if (drop_count == 0) return;

  const bool rt = frame.rt_shadows && static_cast<bool>(pipeline_rt_) && raytracing &&
                  raytracing->tlas(tlas_slot);

  graph.AddPass(
      "precip_volume",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Read(depth, ResourceUsage::kSampledFragment);
      },
      [this, color, depth, motion, raytracing, tlas_slot, frame, drop_count, splash_count,
       rt](PassContext& ctx) {
        PrecipPush push{};
        push.view_proj = frame.view_proj;
        push.prev_view_proj = frame.prev_view_proj;
        push.cam_right[0] = frame.cam_right.x;
        push.cam_right[1] = frame.cam_right.y;
        push.cam_right[2] = frame.cam_right.z;
        push.time = frame.time;
        push.cam_up[0] = frame.cam_up.x;
        push.cam_up[1] = frame.cam_up.y;
        push.cam_up[2] = frame.cam_up.z;
        push.intensity = frame.intensity;
        push.cam_pos[0] = frame.cam_pos.x;
        push.cam_pos[1] = frame.cam_pos.y;
        push.cam_pos[2] = frame.cam_pos.z;
        push.flags = (frame.snow ? 1u : 0u) | (frame.froxel_enabled ? 2u : 0u);
        push.sun_dir[0] = frame.sun_direction.x;
        push.sun_dir[1] = frame.sun_direction.y;
        push.sun_dir[2] = frame.sun_direction.z;
        push.sun_intensity = frame.sun_intensity;
        push.sun_color[0] = frame.sun_color.x;
        push.sun_color[1] = frame.sun_color.y;
        push.sun_color[2] = frame.sun_color.z;
        push.ambient = frame.ambient;
        push.wind[0] = frame.wind[0];
        push.wind[1] = frame.wind[1];
        push.wind[2] = frame.gustiness;
        push.wind[3] = frame.lightning;
        std::memcpy(push.occl, frame.occl, sizeof(push.occl));
        push.jitter[0] = frame.jitter[0];
        push.jitter[1] = frame.jitter[1];
        push.dt = frame.dt;
        push.occl_range = frame.occl_range;

        const GpuImage& target = ctx.graph->image(color);
        ColorAttachment attachments[2];
        attachments[0] = {.view = target.view, .load = LoadOp::kLoad};
        attachments[1] = {.view = ctx.graph->image(motion).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = target.extent, .colors = attachments});

        // The occlusion map sits in kShaderReadAll; the froxel volume stays in
        // GENERAL like every other consumer of it. When froxel fog never
        // initialized its view is null - the stand-in keeps the descriptor
        // valid (the push flag already zeroes the term).
        const TextureView froxel_view =
            frame.froxel_volume ? frame.froxel_volume : froxel_dummy_.view;
        const SamplerHandle froxel_sampler =
            frame.froxel_sampler ? frame.froxel_sampler : frame.occlusion_sampler;
        ctx.cmd->BindPipeline(rt ? pipeline_rt_ : pipeline_);
        base::Vector<BindingItem> items;
        items.push_back(Bind::Combined(0, frame.occlusion, frame.occlusion_sampler));
        items.push_back(Bind::Sampled(1, ctx.graph->image(depth)));
        items.push_back(InGeneral(Bind::Combined(2, froxel_view, froxel_sampler)));
        if (rt) items.push_back(Bind::Accel(3, raytracing->tlas(tlas_slot)));
        ctx.cmd->BindTransient(0, {items.data(), items.size()});
        ctx.cmd->Push(push);
        ctx.cmd->Draw(4, drop_count, 0, 0);

        if (splash_count > 0) {
          ctx.cmd->BindPipeline(splash_pipeline_);
          ctx.cmd->BindTransient(
              0, {Bind::Combined(0, frame.occlusion, frame.occlusion_sampler),
                  Bind::Sampled(1, ctx.graph->image(depth)),
                  InGeneral(Bind::Combined(2, froxel_view, froxel_sampler))});
          ctx.cmd->Push(push);
          ctx.cmd->Draw(4, splash_count, 0, 0);
        }
        ctx.cmd->EndRendering();
      });
}

}  // namespace rx::render
