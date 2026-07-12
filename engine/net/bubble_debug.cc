#include "net/bubble_debug.h"

#include <base/option.h>

#include "core/log.h"
#include "core/math.h"
#include "net/bubble.h"
#include "render/rhi/vulkan_interop.h"
#include "render/util/shader_util.h"

#include "shaders/bubble_wire_ps_hlsl.h"
#include "shaders/bubble_wire_vs_hlsl.h"

namespace rx::net {
namespace {

base::Option<bool> BubblesOpt{"net.bubbles", true, "RX_NET_BUBBLES"};

// Mirrors the HLSL Push block in bubble_wire.vs.hlsl.
struct BubblePush {
  Mat4 view_proj;
  f32 center[3];
  f32 radius;
  f32 color[4];
  f32 jitter[2];
  u32 segments;
  f32 pad;
};
static_assert(sizeof(BubblePush) == 112, "push layout must match bubble_wire.vs.hlsl");

constexpr VkShaderStageFlags kPushStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
constexpr u32 kRingSegments = 96;

}  // namespace

BubbleVisualizer::~BubbleVisualizer() { Shutdown(); }

bool BubbleVisualizer::Init(render::Renderer& renderer) {
  renderer_ = &renderer;
  device_ = renderer.device();
  if (!device_) return false;
  const render::VulkanHandles vk = render::GetVulkanHandles(*device_);
  if (vk.device == VK_NULL_HANDLE) {
    RX_WARN("net: bubble visualizer needs the vulkan backend, staying inert");
    return false;
  }
  vk_ = vk.device;
  ready_ = true;
  return true;
}

bool BubbleVisualizer::BuildPipeline(const render::SceneHookContext& ctx) {
  VkPushConstantRange range{kPushStages, 0, sizeof(BubblePush)};
  VkPipelineLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lci.pushConstantRangeCount = 1;
  lci.pPushConstantRanges = &range;
  if (vkCreatePipelineLayout(vk_, &lci, nullptr, &layout_) != VK_SUCCESS) return false;

  VkShaderModule vs =
      render::CreateShaderModule(vk_, k_bubble_wire_vs_hlsl, sizeof(k_bubble_wire_vs_hlsl));
  VkShaderModule ps =
      render::CreateShaderModule(vk_, k_bubble_wire_ps_hlsl, sizeof(k_bubble_wire_ps_hlsl));
  if (!vs || !ps) {
    if (vs) vkDestroyShaderModule(vk_, vs, nullptr);
    if (ps) vkDestroyShaderModule(vk_, ps, nullptr);
    return false;
  }

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
      .topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST};
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
  // Depth-tested against rx geometry (reversed-z) so bubbles read as sitting
  // in the world, but never written: wireframe must not occlude anything.
  VkPipelineDepthStencilStateCreateInfo ds{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = VK_TRUE,
      .depthWriteEnable = VK_FALSE,
      .depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL};
  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend};
  VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
      .dynamicStateCount = 2,
      .pDynamicStates = dyn_states};

  // The transparent-phase targets exactly as the hook reports them.
  VkFormat color_format = render::GetVkFormat(ctx.color_format);
  VkPipelineRenderingCreateInfo rendering{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
      .colorAttachmentCount = 1,
      .pColorAttachmentFormats = &color_format,
      .depthAttachmentFormat = render::GetVkFormat(ctx.depth_format)};

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
  vkCreateGraphicsPipelines(vk_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline_);

  vkDestroyShaderModule(vk_, vs, nullptr);
  vkDestroyShaderModule(vk_, ps, nullptr);
  return pipeline_ != VK_NULL_HANDLE;
}

void BubbleVisualizer::Record(const render::SceneHookContext& ctx) {
  if (!pipeline_ && !pipeline_failed_) {
    if (!BuildPipeline(ctx)) {
      pipeline_failed_ = true;
      RX_ERROR("net: bubble visualizer pipeline failed, overlay disabled");
    }
  }
  if (!pipeline_) return;

  VkCommandBuffer cb = render::GetVkCommandBuffer(*ctx.cmd);
  render::ColorAttachment color{.view = ctx.color_view, .load = render::LoadOp::kLoad};
  render::DepthAttachment depth{.view = ctx.depth_view, .load = render::LoadOp::kLoad};
  ctx.cmd->BeginRendering({.extent = ctx.extent, .colors = {&color, 1}, .depth = &depth});
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

  BubblePush push{};
  push.view_proj = ctx.view_proj;
  push.jitter[0] = ctx.jitter[0];
  push.jitter[1] = ctx.jitter[1];
  push.segments = kRingSegments;
  for (const BubbleState& b : bubbles_) {
    // Pushed into the HDR scene before tonemapping, so the lines are emissive
    // (well above 1.0) to survive exposure and read at a distance.
    constexpr f32 kEmissive = 8.0f;
    const u32 rgb = PeerColor(b.peer);
    push.color[0] = static_cast<f32>((rgb >> 16) & 0xff) / 255.0f * kEmissive;
    push.color[1] = static_cast<f32>((rgb >> 8) & 0xff) / 255.0f * kEmissive;
    push.color[2] = static_cast<f32>(rgb & 0xff) / 255.0f * kEmissive;
    push.color[3] = 0.9f;
    for (int i = 0; i < 3; ++i) push.center[i] = b.center[i];
    push.radius = b.radius;
    vkCmdPushConstants(cb, layout_, kPushStages, 0, sizeof(push), &push);
    vkCmdDraw(cb, kRingSegments * 2 * 3, 1, 0, 0);  // 3 rings, line list
  }
  ctx.cmd->EndRendering();
}

void BubbleVisualizer::Emit(render::FrameView& view,
                            const base::Vector<BubbleState>& bubbles) {
  if (!ready_ || pipeline_failed_ || bubbles.size() == 0) return;
  if (!BubblesOpt.get()) return;
  bubbles_.clear();
  for (const BubbleState& b : bubbles) bubbles_.push_back(b);
  // Compose with whatever transparent-phase pass the app already installed.
  auto previous = std::move(view.scene_transparent);
  view.scene_transparent = [this, previous](const render::SceneHookContext& ctx) {
    if (previous) previous(ctx);
    Record(ctx);
  };
}

void BubbleVisualizer::Shutdown() {
  if (!device_) return;
  if (vk_ != VK_NULL_HANDLE) {
    device_->WaitIdle();
    if (pipeline_) vkDestroyPipeline(vk_, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(vk_, layout_, nullptr);
  }
  pipeline_ = VK_NULL_HANDLE;
  layout_ = VK_NULL_HANDLE;
  vk_ = VK_NULL_HANDLE;
  device_ = nullptr;
  ready_ = false;
  pipeline_failed_ = false;
}

}  // namespace rx::net
