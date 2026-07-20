#include "render/atmosphere/lightning.h"

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "render/core/settings.h"
#include "render/pipeline/mesh_pipeline.h"
#include "shaders/lightning_bolt_ps_hlsl.h"
#include "shaders/lightning_bolt_vs_hlsl.h"

namespace rx::render {
namespace {

constexpr Format kLightningMotionFormat = Format::kRG16Float;  // == kMotionFormat

// Mirrors LightningPush in lightning_common.hlsli.
struct LightningPush {
  Mat4 view_proj;
  f32 cam_pos[3]; f32 time;
  f32 strike_pos[3]; f32 age;
  u32 seed; f32 energy; f32 jitter[2];
};
static_assert(sizeof(LightningPush) == 112);

// Segment instances: 128 main-channel segments (7 midpoint-displacement
// octaves) + 5 branches x 24 segments; inactive branches collapse in the VS.
constexpr u32 kMainSegments = 128;
constexpr u32 kBranchCount = 5;
constexpr u32 kBranchSegments = 24;
constexpr u32 kSegmentInstances = kMainSegments + kBranchCount * kBranchSegments;

// One positioned flash light per strike, tuned on the weather demo. The
// clustered path attenuates with the windowed (1 - d^2/r^2)^2 falloff - NOT
// inverse-square - so intensity is effectively surface radiance: ~0.68 of it
// survives directly under the bolt, fading to zero at the radius. ~26 puts
// the ground flash a bit above the staged storm sun (1.3) at full energy.
constexpr f32 kFlashHeight = 250.0f;
constexpr f32 kFlashRadius = 600.0f;
constexpr f32 kFlashIntensity = 26.0f;

}  // namespace

f32 LightningSystem::Envelope(f32 age, u32 seed) {
  return LightningEnvelope(age, seed);
}

bool LightningSystem::Initialize(Device& device, Format color_format) {
  // Slot 0 is the prepass depth (pixel stage, soft occlusion test).
  base::Vector<PipelineBindings> sets;
  sets.push_back({.slots = {{0, BindingType::kSampledImage}}});

  // Attachment 0 = lit colour (additive: the bolt only adds energy), 1 =
  // motion (alpha-weighted: the solid core pins zero motion for TAA).
  GraphicsPipelineDesc desc{
      .vertex = RX_SHADER(k_lightning_bolt_vs_hlsl),
      .fragment = RX_SHADER(k_lightning_bolt_ps_hlsl),
      .topology = PrimitiveTopology::kTriangleStrip,
      .raster = {.cull = CullMode::kNone},
      .color_formats = {color_format, kLightningMotionFormat},
      .blend = {BlendMode::kAdditive, BlendMode::kAlpha},
      .sets = sets,
      .push_constant_size = sizeof(LightningPush),
      .debug_name = "lightning_bolt",
  };
  pipeline_ = device.CreateGraphicsPipeline(desc);
  if (!pipeline_) {
    RX_ERROR("lightning bolt pipeline creation failed");
    return false;
  }
  return true;
}

void LightningSystem::Destroy(Device& device) {
  device.DestroyPipeline(pipeline_);
  pipeline_ = {};
}

u32 LightningSystem::AppendLights(PointLight* dst, u32 remaining_capacity,
                                  const WeatherSettings& weather) const {
  if (remaining_capacity == 0) return 0;
  if (weather.strike_age < 0.0f || weather.strike_age >= kStrikeDuration) return 0;
  f32 env = Envelope(weather.strike_age, weather.strike_seed);
  f32 intensity = env * std::clamp(weather.strike_energy, 0.0f, 1.0f) * kFlashIntensity;
  if (intensity <= 1.0f) return 0;
  // One cool blue-white omni at the channel's mid height. Because it rides
  // the normal frame-light path it clusters, claims local-shadow faces, fills
  // the froxel volumetrics and is seen by ReSTIR/clustered reflections - the
  // wet ground flashes even though the bolt itself is a raster overlay (the
  // deliberate trade: no TLAS geometry for a sub-half-second effect, the
  // flash light + bloom stand in for the bolt in reflections).
  PointLight l;
  l.pos_radius[0] = weather.strike_pos.x;
  l.pos_radius[1] = weather.strike_pos.y + kFlashHeight;
  l.pos_radius[2] = weather.strike_pos.z;
  l.pos_radius[3] = kFlashRadius;
  l.color_intensity[0] = 0.62f;
  l.color_intensity[1] = 0.70f;
  l.color_intensity[2] = 1.00f;
  l.color_intensity[3] = intensity;
  dst[0] = l;
  return 1;
}

void LightningSystem::AddToGraph(RenderGraph& graph, ResourceHandle color, ResourceHandle depth,
                                 ResourceHandle motion, const Frame& frame) {
  if (!available()) return;
  if (frame.strike_age < 0.0f || frame.strike_age >= kStrikeDuration) return;

  graph.AddPass(
      "lightning_bolt",
      [&](RenderGraph::PassBuilder& builder) {
        builder.Write(color, ResourceUsage::kColorAttachment);
        builder.Write(motion, ResourceUsage::kColorAttachment);
        builder.Read(depth, ResourceUsage::kSampledFragment);
      },
      [this, color, depth, motion, frame](PassContext& ctx) {
        LightningPush push{};
        push.view_proj = frame.view_proj;
        push.cam_pos[0] = frame.cam_pos.x;
        push.cam_pos[1] = frame.cam_pos.y;
        push.cam_pos[2] = frame.cam_pos.z;
        push.time = frame.time;
        push.strike_pos[0] = frame.strike_pos.x;
        push.strike_pos[1] = frame.strike_pos.y;
        push.strike_pos[2] = frame.strike_pos.z;
        push.age = frame.strike_age;
        push.seed = frame.strike_seed;
        push.energy = std::clamp(frame.strike_energy, 0.0f, 1.0f);
        push.jitter[0] = frame.jitter[0];
        push.jitter[1] = frame.jitter[1];

        const GpuImage& target = ctx.graph->image(color);
        ColorAttachment attachments[2];
        attachments[0] = {.view = target.view, .load = LoadOp::kLoad};
        attachments[1] = {.view = ctx.graph->image(motion).view, .load = LoadOp::kLoad};
        ctx.cmd->BeginRendering({.extent = target.extent, .colors = attachments});
        ctx.cmd->BindPipeline(pipeline_);
        ctx.cmd->BindTransient(0, {Bind::Sampled(0, ctx.graph->image(depth))});
        ctx.cmd->Push(push);
        ctx.cmd->Draw(4, kSegmentInstances, 0, 0);
        ctx.cmd->EndRendering();
      });
}

}  // namespace rx::render
