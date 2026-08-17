#ifndef RX_RENDER_RHI_PIPELINE_H_
#define RX_RENDER_RHI_PIPELINE_H_

#include <base/containers/vector.h>

#include "core/types.h"
#include "render/rhi/bindings.h"
#include "render/rhi/types.h"

namespace rx::render {

// The portability floor for the Vulkan 1.0-1.3 devices rx targets, Android
// included: maxPushConstantsSize is guaranteed to be *at least* this and
// nothing more. It is not a mobile-only concern and the split is not
// desktop-versus-mobile - plenty of desktop AMD parts report exactly 128,
// while several Adreno and Mali report 256. A bigger block runs fine on
// whatever adapter it was written against and then fails pipeline-layout
// creation elsewhere, silently costing the whole pass.
inline constexpr u32 kGuaranteedPushConstantBytes = 128;

// Every desc states its push block through this, so an oversized struct is a
// build error at the pipeline that asks for it rather than a runtime failure
// on hardware we do not own. The device still re-checks against the adapter's
// real limit, for the handful of sizes that are only known at runtime.
template <typename T>
consteval u32 PushSize() {
  static_assert(sizeof(T) <= kGuaranteedPushConstantBytes,
                "push constant block is over the 128-byte portability floor; "
                "move the overflow (usually the matrices) into a uniform buffer");
  return sizeof(T);
}

// Deliberate exception, for a block whose every field is genuinely per-draw:
// there is nothing to lift into a per-frame uniform, so fitting it means
// routing the per-draw data through an indexed buffer - a redesign of the pass,
// not a repack. Named rather than a bare sizeof so the exception stays
// greppable and cannot be reached by accident; the device still refuses the
// layout (and says which pipeline) on an adapter that cannot serve it.
template <typename T>
consteval u32 PushSizeOverGuarantee() {
  static_assert(sizeof(T) > kGuaranteedPushConstantBytes,
                "block fits the guarantee - use PushSize<> instead");
  return sizeof(T);
}

// A pipeline's descriptor-set interface. Most passes declare their slots
// inline and let the device derive (and cache) the layout; sets shared across
// pipelines (bindless registry, frame globals) pass the existing layout
// handle instead.
struct PipelineBindings {
  base::Vector<BindingSlot> slots;       // inline definition, or
  BindingLayoutHandle shared;            // an externally created layout
  ShaderStageFlags stages = kShaderStageNone;  // 0 = all stages of the pipeline
};

struct ComputePipelineDesc {
  ShaderBlob shader;
  base::Vector<PipelineBindings> sets;
  u32 push_constant_size = 0;
  const char* debug_name = nullptr;
};

struct VertexAttribute {
  u32 location = 0;
  Format format = Format::kRGB32Float;
  u32 offset = 0;
};

struct VertexBufferLayout {
  u32 stride = 0;
  bool per_instance = false;
  base::Vector<VertexAttribute> attributes;
};

struct DepthState {
  bool test = false;
  bool write = false;
  CompareOp compare = CompareOp::kGreaterEqual;  // reversed-z default
  Format format = Format::kUnknown;              // kUnknown = no depth attachment
  f32 bias_constant = 0.0f;
  f32 bias_slope = 0.0f;
};

struct RasterState {
  CullMode cull = CullMode::kBack;
  FrontFace front = FrontFace::kCounterClockwise;
  PolygonMode polygon = PolygonMode::kFill;
};

// Dynamic-rendering only: attachment formats are part of the pipeline, render
// targets bind at record time. Viewport/scissor are always dynamic.
struct GraphicsPipelineDesc {
  ShaderBlob vertex;    // exclusive with task/mesh
  ShaderBlob fragment;
  ShaderBlob task;      // mesh-shader path
  ShaderBlob mesh;
  base::Vector<VertexBufferLayout> vertex_buffers;
  PrimitiveTopology topology = PrimitiveTopology::kTriangleList;
  RasterState raster;
  DepthState depth;
  base::Vector<Format> color_formats;
  base::Vector<BlendMode> blend;  // per color target; empty = all opaque
  base::Vector<PipelineBindings> sets;
  u32 push_constant_size = 0;
  // Rasterization sample count; must match the bound targets' samples.
  // 1 = the standard single-sampled path (every existing pipeline).
  u32 samples = 1;
  const char* debug_name = nullptr;
};

}  // namespace rx::render

#endif  // RX_RENDER_RHI_PIPELINE_H_
