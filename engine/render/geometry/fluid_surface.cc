#include "render/geometry/fluid_surface.h"

#include <algorithm>

#include "core/log.h"
#include "render/pipeline/mesh_pipeline.h"
#include "shaders/fluid_surface_ps_hlsl.h"
#include "shaders/fluid_surface_vs_hlsl.h"

namespace rx::render {
namespace {

// Shading knobs, mirrored by FluidSurfacePush in fluid_surface.vs/ps.hlsl. The
// solver's world/grid mapping lives in the separate params CB (fluid_sim.h
// GpuParams); this block only carries surface look tunables.
struct FluidSurfacePush {
  f32 eps = 2.0e-3f;            // min fluid depth to draw (m); dry cells clip
  f32 time = 0.0f;             // seconds, drives the flow-map phases
  f32 water_absorption = 2.6f;  // Beer coeff mapping eye depth -> opacity (1/m)
  f32 foam_scale = 0.28f;      // flow-map advection distance (m per m/s)
  f32 absorb_color[4] = {0.14f, 0.42f, 0.50f, 1.0f};  // deep-water blue-green tint
  f32 flow_period = 1.7f;      // s, flow-map re-seed period T (Vlachos)
  f32 foam_speed_lo = 0.55f;   // m/s: foam onset (calm/settled water stays clear)
  f32 foam_speed_hi = 2.6f;    // m/s: full white water at the dam-break front
  f32 lava_emissive = 7.0f;    // HDR scale: hot core blooms without whiting out
  u32 grid = 0;                // cells per side of THIS draw (VS cell decode
                               // must match Draw()'s vertex count exactly)
  u32 pad[3] = {};
};
static_assert(sizeof(FluidSurfacePush) == 64);

// Grid resolution the surface rasterises at. The solver texture may be up to
// 1024^2, but 512^2 cells (~1.5M verts, x2 instances) already over-tessellates
// the domain on screen; a coarser grid samples the state bilinearly.
constexpr u32 kMaxGridResolution = 512;

}  // namespace

std::unique_ptr<FluidSurfacePass> FluidSurfacePass::Create(
    Device& device, Format color_format, Format motion_format, Format depth_format,
    BindingLayoutHandle globals_layout, BindingLayoutHandle environment_layout,
    BindingLayoutHandle bindless_layout) {
  (void)bindless_layout;  // IBL rides the environment set; no bindless table here.
  auto pass = std::unique_ptr<FluidSurfacePass>(new FluidSurfacePass(device));

  // Linear + clamp: the state/bed/velocity fields are sampled continuously
  // across the (coarser) render grid and must not wrap at the domain edge.
  pass->sampler_ = device.GetSampler({.address_u = AddressMode::kClampToEdge,
                                      .address_v = AddressMode::kClampToEdge,
                                      .address_w = AddressMode::kClampToEdge});

  // Mirrors the transparent pass attachments (scene color + motion + depth) so
  // the render pass stays compatible with WaterPass. Alpha blend over the scene
  // (shallow water reads through for free; lava writes alpha 1). Depth write on
  // so the lower lava surface and the higher water surface resolve regardless of
  // raster order (reversed-z Greater).
  pass->pipeline_ = device.CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_fluid_surface_vs_hlsl),
      .fragment = RX_SHADER(k_fluid_surface_ps_hlsl),
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true,
                .write = true,
                .compare = CompareOp::kGreater,  // reversed z
                .format = depth_format},
      .color_formats = {color_format, motion_format},
      .blend = {BlendMode::kAlpha, BlendMode::kAlpha},
      .sets = {{.shared = globals_layout},
               // Set 1: our transient fluid set (VS lifts the grid from state +
               // bed + params; PS additionally reads velocity for flow foam).
               {.slots = {{0, BindingType::kCombinedTextureSampler},   // state RGBA32F
                          {1, BindingType::kCombinedTextureSampler},   // bed R32F
                          {2, BindingType::kCombinedTextureSampler},   // velocity RGBA16F
                          {3, BindingType::kUniformBuffer}},           // params CB
                .stages = kShaderStageVertex | kShaderStageFragment},
               {.shared = environment_layout}},
      .push_constant_size = sizeof(FluidSurfacePush),
      .debug_name = "fluid_surface",
  });
  if (!pass->pipeline_) {
    RX_ERROR("fluid surface pipeline creation failed");
    return nullptr;
  }
  return pass;
}

FluidSurfacePass::~FluidSurfacePass() {
  if (pipeline_) device_.DestroyPipeline(pipeline_);
}

void FluidSurfacePass::Draw(PassContext& ctx, BindingSetHandle globals,
                            BindingSetHandle environment, const FluidSim& sim, u32 frame_slot,
                            f32 time) {
  const u32 grid = std::min(sim.domain().resolution, kMaxGridResolution);
  if (grid == 0) return;

  ctx.cmd->BindPipeline(pipeline_);
  ctx.cmd->BindSet(0, globals);
  ctx.cmd->BindSet(2, environment);
  // The solver's images live in GENERAL (storage read/write); sample them there
  // (InGeneral) exactly like the water-field rings the transparent env set
  // wraps. The params CB is the current frame slot's copy the solver filled.
  ctx.cmd->BindTransient(
      1, {InGeneral(Bind::Combined(0, sim.state_view(), sampler_)),
          InGeneral(Bind::Combined(1, sim.bed_view(), sampler_)),
          InGeneral(Bind::Combined(2, sim.velocity_view(), sampler_)),
          Bind::Uniform(3, sim.params_buffer(frame_slot))});

  FluidSurfacePush push;
  push.time = time;
  push.grid = grid;
  ctx.cmd->PushConstants(&push, sizeof(push));

  // Two triangles per cell, N*N cells, x2 instances (water then lava). Dry cells
  // collapse to a degenerate position in the VS, so this is cheap where empty.
  ctx.cmd->Draw(6u * grid * grid, 2u);
}

}  // namespace rx::render
