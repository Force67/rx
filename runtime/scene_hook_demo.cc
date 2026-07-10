#include "scene_hook_demo.h"

#include <cstring>

#include "core/log.h"
#include "render/rhi/vulkan_interop.h"
#include "render/util/shader_util.h"

#include "shaders/scenehook_cull_cs_hlsl.h"
#include "shaders/scenehook_ps_hlsl.h"
#include "shaders/scenehook_vs_hlsl.h"

namespace rx {

namespace {

// Mirrors the HLSL Push block (scenehook_cull.cs / scenehook.vs). uint64_t sits
// at offset 64 (8-byte aligned after the 4x4), so the natural C++ layout matches
// the SPIR-V push-constant layout dxc emits.
struct ScenePush {
  Mat4 view_proj;
  u64 addr;
  f32 jitter[2];
  u32 count;
  f32 time;
};
static_assert(sizeof(ScenePush) == 88, "push layout must match the demo shaders");

constexpr VkShaderStageFlags kPushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

}  // namespace

SceneHookDemo::~SceneHookDemo() { Shutdown(); }

bool SceneHookDemo::Init(render::Renderer& renderer) {
  renderer_ = &renderer;
  device_ = renderer.device();
  if (!device_) return false;
  const render::VulkanHandles vk = render::GetVulkanHandles(*device_);
  if (vk.device == VK_NULL_HANDLE) {
    RX_WARN("scenehook demo: not on the vulkan backend, staying inert");
    return false;
  }
  vk_ = vk.device;

  // One push-constant range shared by both pipelines; no descriptor sets (the
  // instance arena is reached purely by device address).
  VkPushConstantRange range{kPushStages, 0, sizeof(ScenePush)};
  VkPipelineLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lci.pushConstantRangeCount = 1;
  lci.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(vk_, &lci, nullptr, &layout_) != VK_SUCCESS) return false;

  VkShaderModule cs = render::CreateShaderModule(vk_, k_scenehook_cull_cs_hlsl,
                                                 sizeof(k_scenehook_cull_cs_hlsl));
  VkShaderModule vs =
      render::CreateShaderModule(vk_, k_scenehook_vs_hlsl, sizeof(k_scenehook_vs_hlsl));
  VkShaderModule ps =
      render::CreateShaderModule(vk_, k_scenehook_ps_hlsl, sizeof(k_scenehook_ps_hlsl));
  if (!cs || !vs || !ps) {
    if (cs) vkDestroyShaderModule(vk_, cs, nullptr);
    if (vs) vkDestroyShaderModule(vk_, vs, nullptr);
    if (ps) vkDestroyShaderModule(vk_, ps, nullptr);
    return false;
  }

  // Compute placement/cull pipeline.
  {
    VkPipelineShaderStageCreateInfo stage{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = cs,
        .pName = "main"};
    VkComputePipelineCreateInfo cpci{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = stage;
    cpci.layout = layout_;
    vkCreateComputePipelines(vk_, VK_NULL_HANDLE, 1, &cpci, nullptr, &compute_);
  }

  // Graphics pipeline: dynamic rendering into rx's scene colour + depth-export,
  // reversed-z depth-tested against rx geometry.
  {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = ps,
         .pName = "main"}};

    VkPipelineVertexInputStateCreateInfo vin{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL};  // reversed-z
    VkPipelineColorBlendAttachmentState blend[2];
    for (auto& b : blend) {
      b = {};
      b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = blend};
    VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states};

    // Attachment formats match rx's scene_color (RGBA16F), depth_export (R32F)
    // and depth (D32) - stable transient targets the opaque hook exposes.
    VkFormat color_formats[2] = {render::GetVkFormat(render::Format::kRGBA16Float),
                                 render::GetVkFormat(render::Format::kR32Float)};
    VkPipelineRenderingCreateInfo rendering{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 2,
        .pColorAttachmentFormats = color_formats,
        .depthAttachmentFormat = render::GetVkFormat(render::Format::kD32Float)};

    VkGraphicsPipelineCreateInfo gpci{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpci.pNext = &rendering;
    gpci.stageCount = 2;
    gpci.pStages = stages;
    gpci.pVertexInputState = &vin;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState = &ms;
    gpci.pDepthStencilState = &ds;
    gpci.pColorBlendState = &cb;
    gpci.pDynamicState = &dyn;
    gpci.layout = layout_;
    vkCreateGraphicsPipelines(vk_, VK_NULL_HANDLE, 1, &gpci, nullptr, &graphics_);
  }

  vkDestroyShaderModule(vk_, cs, nullptr);
  vkDestroyShaderModule(vk_, vs, nullptr);
  vkDestroyShaderModule(vk_, ps, nullptr);
  if (!compute_ || !graphics_) return false;

  // A BDA arena per frame-in-flight: the compute pass rewrites the slot's arena
  // every frame before the draw reads it, and the slot only recycles once its
  // fence has fired, so no cross-frame hazard.
  u32 frames = render::GetVulkanFramesInFlight(*device_);
  if (frames == 0) frames = 1;
  const u64 arena_bytes = static_cast<u64>(instance_count_) * 32u;  // float4 pos + float4 colour
  for (u32 i = 0; i < frames; ++i) {
    render::GpuBuffer arena = device_->CreateBuffer(
        arena_bytes,
        render::kBufferUsageStorage | render::kBufferUsageDeviceAddress, /*host_visible=*/false);
    if (!arena.address) {
      RX_ERROR("scenehook demo: buffer-device-address unavailable");
      return false;
    }
    arenas_.push_back(arena);
  }

  ready_ = true;
  RX_INFO("scenehook demo ready: {} instanced boxes via a compute-filled BDA arena",
          instance_count_);
  return true;
}

void SceneHookDemo::Record(const render::SceneHookContext& ctx) {
  VkCommandBuffer cb = render::GetVkCommandBuffer(*ctx.cmd);
  const render::GpuBuffer& arena = arenas_[ctx.frame_slot % arenas_.size()];

  ScenePush push{};
  push.view_proj = ctx.view_proj;
  push.addr = arena.address;
  push.jitter[0] = ctx.jitter[0];
  push.jitter[1] = ctx.jitter[1];
  push.count = instance_count_;
  push.time = time_;

  // 1) Compute placement/cull writing the arena by device address.
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, compute_);
  vkCmdPushConstants(cb, layout_, kPushStages, 0, sizeof(push), &push);
  vkCmdDispatch(cb, (instance_count_ + 63) / 64, 1, 1);
  // Compute storage write -> vertex-stage BDA read hazard.
  ctx.cmd->MemoryBarrier(render::BarrierScope::kComputeWrite, render::BarrierScope::kGraphicsRead);

  // 2) Open a dynamic-rendering section on rx's scene targets and draw. LoadOp
  // kLoad preserves rx's opaque + sky; the graph already put the images in their
  // attachment layouts.
  render::ColorAttachment colors[2];
  colors[0] = {.view = ctx.color_view, .load = render::LoadOp::kLoad};
  colors[1] = {.view = ctx.depth_export_view, .load = render::LoadOp::kLoad};
  render::DepthAttachment depth{.view = ctx.depth_view, .load = render::LoadOp::kLoad};
  ctx.cmd->BeginRendering(
      {.extent = ctx.extent, .colors = {colors, 2}, .depth = &depth});
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_);
  vkCmdPushConstants(cb, layout_, kPushStages, 0, sizeof(push), &push);
  vkCmdDraw(cb, 36, instance_count_, 0, 0);  // 12 tris per box, instanced
  ctx.cmd->EndRendering();
}

void SceneHookDemo::Emit(f32 dt, render::FrameView& view) {
  if (!ready_) return;
  time_ += dt;
  view.scene_opaque = [this](const render::SceneHookContext& ctx) { Record(ctx); };
}

void SceneHookDemo::Shutdown() {
  if (!device_) return;
  if (vk_ != VK_NULL_HANDLE) {
    device_->WaitIdle();
    if (graphics_) vkDestroyPipeline(vk_, graphics_, nullptr);
    if (compute_) vkDestroyPipeline(vk_, compute_, nullptr);
    if (layout_) vkDestroyPipelineLayout(vk_, layout_, nullptr);
  }
  for (render::GpuBuffer& arena : arenas_) device_->DestroyBuffer(arena);
  arenas_.clear();
  graphics_ = compute_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  vk_ = VK_NULL_HANDLE;
  device_ = nullptr;
  ready_ = false;
}

}  // namespace rx
