#include "scene_hook_rhi_demo.h"

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "render/rhi/bindings.h"

#include "shaders/scenehook_rhi_cull_cs_hlsl.h"
#include "shaders/scenehook_rhi_ms_hlsl.h"
#include "shaders/scenehook_rhi_ps_hlsl.h"
#include "shaders/scenehook_rhi_vs_hlsl.h"

namespace rx {

namespace {

// Mirrors the HLSL Push block shared by the cull/draw/mesh shaders. The u64s sit
// 8-byte aligned after the 64-byte matrix so the natural C++ layout matches the
// SPIR-V push-constant layout dxc emits.
struct RhiPush {
  Mat4 view_proj;
  u64 instance_addr;
  u64 args_addr;
  u64 count_addr;
  u64 churn_addr;
  f32 jitter[2];
  u32 count;
  f32 time;
  u32 layer_count;
  u32 pad;
};
static_assert(sizeof(RhiPush) == 120, "push layout must match the demo shaders");

render::ShaderBlob Spirv(const unsigned char* data, size_t size) {
  return render::ShaderBlob{data, size, render::ShaderFormat::kSpirv};
}

constexpr u32 kTexSize = 8;

}  // namespace

SceneHookRhiDemo::~SceneHookRhiDemo() { Shutdown(); }

bool SceneHookRhiDemo::CreateTextureArray() {
  tex_array_ = device_->CreateImage2DArray(
      render::Format::kRGBA8Unorm, kTexSize, kTexSize, layer_count_,
      render::kTextureUsageSampled | render::kTextureUsageTransferDst, /*mip_levels=*/1);
  if (!tex_array_) {
    RX_ERROR("scenehook-rhi: 2d texture array creation failed");
    return false;
  }

  // A distinct 2-colour checker per layer, so which array layer a box samples is
  // unmistakable in the capture.
  const f32 base[4][3] = {
      {1.0f, 0.35f, 0.30f}, {0.30f, 1.0f, 0.45f}, {0.35f, 0.55f, 1.0f}, {1.0f, 0.85f, 0.25f}};
  const u32 layer_bytes = kTexSize * kTexSize * 4;
  base::Vector<u8> pixels(static_cast<size_t>(layer_bytes) * layer_count_);
  for (u32 l = 0; l < layer_count_; ++l) {
    const f32* c = base[l % 4];
    for (u32 y = 0; y < kTexSize; ++y) {
      for (u32 x = 0; x < kTexSize; ++x) {
        f32 k = (((x >> 1) ^ (y >> 1)) & 1u) ? 1.0f : 0.30f;
        u8* p = &pixels[static_cast<size_t>(l) * layer_bytes + (y * kTexSize + x) * 4];
        p[0] = static_cast<u8>(c[0] * k * 255.0f);
        p[1] = static_cast<u8>(c[1] * k * 255.0f);
        p[2] = static_cast<u8>(c[2] * k * 255.0f);
        p[3] = 255;
      }
    }
  }

  render::GpuBuffer staging =
      device_->CreateBuffer(pixels.size(), render::kBufferUsageTransferSrc, /*host_visible=*/true);
  if (!staging.mapped) {
    RX_ERROR("scenehook-rhi: texture staging buffer failed");
    return false;
  }
  std::memcpy(staging.mapped, pixels.data(), pixels.size());

  device_->ImmediateSubmit([&](render::CommandList& cmd) {
    cmd.Barrier(render::Transition(tex_array_, render::ResourceState::kUndefined,
                                   render::ResourceState::kCopyDst));
    base::Vector<render::BufferTextureCopy> regions(layer_count_);
    for (u32 l = 0; l < layer_count_; ++l) {
      regions[l] = {.buffer_offset = static_cast<u64>(l) * layer_bytes,
                    .mip = 0,
                    .array_layer = l,
                    .extent = {kTexSize, kTexSize}};
    }
    cmd.CopyBufferToTexture(staging, tex_array_, {regions.data(), regions.size()});
    cmd.Barrier(render::Transition(tex_array_, render::ResourceState::kCopyDst,
                                   render::ResourceState::kShaderReadFragment));
  });
  device_->DestroyBuffer(staging);
  return true;
}

bool SceneHookRhiDemo::Init(render::Renderer& renderer) {
  renderer_ = &renderer;
  device_ = renderer.device();
  if (!device_ || device_->is_stub()) return false;
  // The shaders read/write buffer-device-address arenas (SPIR-V only, no DXIL
  // sidecar) and the scene hook fires on Vulkan, so this path is Vulkan-only.
  if (device_->caps().backend != render::Backend::kVulkan) {
    RX_WARN("scenehook-rhi demo: not on the vulkan backend, staying inert");
    return false;
  }

  if (!CreateTextureArray()) return false;
  render::SamplerDesc sd;
  sd.min_filter = sd.mag_filter = sd.mip_filter = render::Filter::kNearest;
  sd.address_u = sd.address_v = render::AddressMode::kClampToEdge;
  sampler_ = device_->GetSampler(sd);

  // Compute cull/placement pipeline: no descriptor sets, everything by address.
  {
    render::ComputePipelineDesc desc;
    desc.shader = Spirv(k_scenehook_rhi_cull_cs_hlsl, sizeof(k_scenehook_rhi_cull_cs_hlsl));
    desc.push_constant_size = sizeof(RhiPush);
    desc.debug_name = "scenehook_rhi_cull";
    cull_pipeline_ = device_->CreateComputePipeline(desc);
  }

  // Shared set 0: the texture array as a combined image sampler in the fragment
  // stage; both draw pipelines declare it identically.
  auto make_set0 = []() {
    render::PipelineBindings set0;
    set0.slots.push_back({.binding = 0, .type = render::BindingType::kCombinedTextureSampler});
    set0.stages = render::kShaderStageFragment;
    return set0;
  };

  // Classic vertex-pulling draw pipeline (DrawIndirectCount path).
  {
    render::GraphicsPipelineDesc desc;
    desc.vertex = Spirv(k_scenehook_rhi_vs_hlsl, sizeof(k_scenehook_rhi_vs_hlsl));
    desc.fragment = Spirv(k_scenehook_rhi_ps_hlsl, sizeof(k_scenehook_rhi_ps_hlsl));
    desc.raster.cull = render::CullMode::kNone;
    desc.depth = {.test = true,
                  .write = true,
                  .compare = render::CompareOp::kGreaterEqual,
                  .format = render::Format::kD32Float};
    desc.color_formats.push_back(render::Format::kRGBA16Float);
    desc.color_formats.push_back(render::Format::kR32Float);
    desc.sets.push_back(make_set0());
    desc.push_constant_size = sizeof(RhiPush);
    desc.debug_name = "scenehook_rhi_draw";
    draw_pipeline_ = device_->CreateGraphicsPipeline(desc);
  }

  // Mesh-shader draw pipeline (DrawMeshTasksIndirect path), when available.
  if (device_->caps().mesh_shaders) {
    render::GraphicsPipelineDesc desc;
    desc.mesh = Spirv(k_scenehook_rhi_ms_hlsl, sizeof(k_scenehook_rhi_ms_hlsl));
    desc.fragment = Spirv(k_scenehook_rhi_ps_hlsl, sizeof(k_scenehook_rhi_ps_hlsl));
    desc.raster.cull = render::CullMode::kNone;
    desc.depth = {.test = true,
                  .write = true,
                  .compare = render::CompareOp::kGreaterEqual,
                  .format = render::Format::kD32Float};
    desc.color_formats.push_back(render::Format::kRGBA16Float);
    desc.color_formats.push_back(render::Format::kR32Float);
    desc.sets.push_back(make_set0());
    desc.push_constant_size = sizeof(RhiPush);
    desc.debug_name = "scenehook_rhi_mesh";
    mesh_pipeline_ = device_->CreateGraphicsPipeline(desc);
    use_mesh_ = static_cast<bool>(mesh_pipeline_);
  }

  if (!cull_pipeline_ || !draw_pipeline_) {
    RX_ERROR("scenehook-rhi: pipeline creation failed");
    return false;
  }

  // One set of GPU-driven buffers per frame-in-flight: the compute pass rewrites
  // the slot every frame before the draw reads it; the slot recycles only once
  // its fence has fired.
  const u64 arena_bytes = static_cast<u64>(instance_count_) * 48u;
  for (u32 i = 0; i < render::Device::kMaxFramesInFlight; ++i) {
    Slot slot;
    slot.instances = device_->CreateBuffer(
        arena_bytes, render::kBufferUsageStorage | render::kBufferUsageDeviceAddress, false);
    slot.args = device_->CreateBuffer(
        64, render::kBufferUsageStorage | render::kBufferUsageIndirect |
                render::kBufferUsageDeviceAddress,
        false);
    slot.count = device_->CreateBuffer(
        16, render::kBufferUsageStorage | render::kBufferUsageIndirect |
                render::kBufferUsageDeviceAddress,
        false);
    if (!slot.instances.address || !slot.args.address || !slot.count.address) {
      RX_ERROR("scenehook-rhi: buffer-device-address unavailable");
      return false;
    }
    slots_.push_back(slot);
  }

  ready_ = true;
  RX_INFO("scenehook-rhi demo ready: {} boxes via compute-written DrawIndirectCount{}",
          instance_count_, use_mesh_ ? " + DrawMeshTasksIndirect" : "");
  return true;
}

void SceneHookRhiDemo::Record(const render::SceneHookContext& ctx) {
  Slot& slot = slots_[ctx.frame_slot % slots_.size()];

  // A tiny scratch buffer the compute pass touches, then churned every frame
  // through the frame-safe deferred-destruction path (exercises the graveyard).
  render::GpuBuffer churn = device_->CreateBuffer(
      256, render::kBufferUsageStorage | render::kBufferUsageDeviceAddress, false);

  RhiPush push{};
  push.view_proj = ctx.view_proj;
  push.instance_addr = slot.instances.address;
  push.args_addr = slot.args.address;
  push.count_addr = slot.count.address;
  push.churn_addr = churn.address;
  push.jitter[0] = ctx.jitter[0];
  push.jitter[1] = ctx.jitter[1];
  push.count = instance_count_;
  push.time = time_;
  push.layer_count = layer_count_;

  // 1) Compute placement/cull: fills the instance arena + the indirect draw/mesh
  // args + the draw count, all by device address.
  ctx.cmd->BindPipeline(cull_pipeline_);
  ctx.cmd->Push(push);
  ctx.cmd->Dispatch((instance_count_ + 63) / 64, 1, 1);
  // Compute storage writes -> indirect-arg fetch and vertex-stage BDA reads.
  ctx.cmd->MemoryBarrier(render::BarrierScope::kComputeWrite, render::BarrierScope::kIndirectArgs);
  ctx.cmd->MemoryBarrier(render::BarrierScope::kComputeWrite, render::BarrierScope::kGraphicsRead);

  // 2) Draw into rx's scene targets (LoadOp kLoad preserves rx's opaque + sky).
  render::ColorAttachment colors[2];
  colors[0] = {.view = ctx.color_view, .load = render::LoadOp::kLoad};
  colors[1] = {.view = ctx.depth_export_view, .load = render::LoadOp::kLoad};
  render::DepthAttachment depth{.view = ctx.depth_view, .load = render::LoadOp::kLoad};
  ctx.cmd->BeginRendering({.extent = ctx.extent, .colors = {colors, 2}, .depth = &depth});

  ctx.cmd->BindPipeline(draw_pipeline_);
  ctx.cmd->BindTransient(0, {render::Bind::Combined(0, tex_array_.view, sampler_)});
  ctx.cmd->Push(push);
  ctx.cmd->DrawIndirectCount(slot.args, 0, slot.count, 0, /*max_draw_count=*/1, /*stride=*/16);

  if (use_mesh_) {
    ctx.cmd->BindPipeline(mesh_pipeline_);
    ctx.cmd->BindTransient(0, {render::Bind::Combined(0, tex_array_.view, sampler_)});
    ctx.cmd->Push(push);
    // One mesh-task record at byte 16 of the args buffer (uint3 group counts).
    ctx.cmd->DrawMeshTasksIndirect(slot.args, 16, /*draw_count=*/1, /*stride=*/12);
  }
  ctx.cmd->EndRendering();

  device_->DestroyBufferDeferred(churn);
}

void SceneHookRhiDemo::Emit(f32 dt, render::FrameView& view) {
  if (!ready_) return;
  time_ += dt;
  view.scene_opaque = [this](const render::SceneHookContext& ctx) { Record(ctx); };
}

void SceneHookRhiDemo::Shutdown() {
  if (!device_) return;
  device_->WaitIdle();
  if (cull_pipeline_) device_->DestroyPipeline(cull_pipeline_);
  if (draw_pipeline_) device_->DestroyPipeline(draw_pipeline_);
  if (mesh_pipeline_) device_->DestroyPipeline(mesh_pipeline_);
  for (Slot& slot : slots_) {
    device_->DestroyBuffer(slot.instances);
    device_->DestroyBuffer(slot.args);
    device_->DestroyBuffer(slot.count);
  }
  slots_.clear();
  if (tex_array_) device_->DestroyImage(tex_array_);
  cull_pipeline_ = draw_pipeline_ = mesh_pipeline_ = {};
  device_ = nullptr;
  ready_ = false;
}

}  // namespace rx
