#include "rxe/render/core/renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <base/check.h>
#include <base/option.h>

#include "base/containers/span.h"
#include "base/containers/vector.h"
#include "base/functional/function.h"
#include "base/memory/mem_ops.h"
#include "base/memory/move.h"
#include "base/memory/unique_pointer.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "base/time/time.h"
#include "foundation/algorithm/sort.h"
#include "foundation/logging/log.h"
#include "foundation/math/scalar.h"
#include "foundation/memory/memory_tracker.h"
#include "rxe/asset/primitives.h"
#include "rxe/asset/texture_compress.h"
#include "rxe/asset/exr_write.h"
#include "rxe/render/core/renderer_internal.h"
#include "shaders/blit_ps_slang.h"
#include "shaders/cloud_shadow_cs_hlsl.h"
#include "shaders/contact_shadow_cs_hlsl.h"
#include "shaders/depth_copy_ps_hlsl.h"
#include "shaders/fullscreen_vs_slang.h"
#include "shaders/hdr_capture_cs_hlsl.h"
#include "shaders/light_cluster_cs_hlsl.h"
#include "shaders/msaa_resolve_cs_hlsl.h"
#include "shaders/sss_blur_cs_hlsl.h"

namespace rx::render {
namespace {

// Renderer config overrides, populated from the environment by
// base::InitOptionsFromEnv() at startup (see Engine::Initialize). DebugView and
// ColorGrade options take an Opt suffix to avoid shadowing the enums of the
// same name used in the casts below.
base::Option<const char *> Screenshot{"screenshot", nullptr, "RX_SCREENSHOT"};

// RX_SEQ=prefix:startsec:count[:stride] dumps a burst of `count` composited
// frames (every `stride`-th presented frame, default 1) starting at startsec,
// named prefix_0000.png ... for stitching into a clip. Inbuilt framebuffer
// capture, same path as the single screenshot.
base::Option<const char *> Sequence{"screenshot.seq", nullptr, "RX_SEQ"};

base::Option<const char *> Hdr{"hdr", nullptr, "RX_HDR"};

base::Option<bool> HdrOutput{"hdr.output", false, "RX_HDR_OUTPUT"};

base::Option<bool> MotionBlurOpt{"motion.blur", true, "RX_MOTION_BLUR"};

base::Option<bool> DofOpt{"dof", true, "RX_DOF"};

base::Option<double> LensFlareOpt{"lens.flare", 0.06, "RX_LENS_FLARE"};

base::Option<double> GrainOpt{"film.grain", 0.0, "RX_FILM_GRAIN"};

base::Option<double> DofFocus{"dof.focus", 0.0, "RX_DOF_FOCUS"};

base::Option<double> DofAperture{"dof.aperture", 2.8, "RX_DOF_APERTURE"};

base::Option<bool> SssOpt{"sss", true, "RX_SSS"};

base::Option<double> SssWidth{"sss.width", 0.012, "RX_SSS_WIDTH"};

base::Option<bool> SkinDynamicsOpt{"skin.dynamics", true, "RX_SKIN_DYNAMICS"};

base::Option<double> SkinHeartRateOpt{"skin.heart_rate", 1.1, "RX_SKIN_HEART_RATE"};

base::Option<double> SkinPerfusionOpt{"skin.perfusion", 0.0, "RX_SKIN_PERFUSION"};

base::Option<double> SkinPulseAmpOpt{"skin.pulse", 0.03, "RX_SKIN_PULSE"};

base::Option<double> SkinTensionGainOpt{"skin.tension", 0.35, "RX_SKIN_TENSION"};

base::Option<bool> AsyncComputeOpt{"async.compute", true, "RX_ASYNC_COMPUTE"};

base::Option<bool> FrameGenOpt{"framegen", false, "RX_FRAMEGEN"};

base::Option<bool> LocalShadowsOpt{"local.shadows", true, "RX_LOCAL_SHADOWS"};

base::Option<bool> FroxelOpt{"froxel.fog", true, "RX_FROXEL"};

base::Option<double> FroxelDensity{"froxel.density", 0.005,
                                   "RX_FROXEL_DENSITY"};

base::Option<double> FroxelStartDistance{"froxel.start", 0.0,
                                         "RX_FROXEL_START"};

base::Option<int> TexBudgetMb{"tex.budget.mb", -1, "RX_TEX_BUDGET_MB"};

// Import-time block compression of material textures. On wherever the device
// can sample BC; the override exists to bisect a suspected compression
// artifact without rebuilding.
base::Option<bool> TexCompressOpt{"tex.compress", true, "RX_TEX_COMPRESS"};

// Extends that to tangent-space normal maps (BC5 + a shader-side z rebuild).
// Off by default; asset/texture_compress.h carries the measurements that put
// it there and what content it is safe on.
base::Option<bool> TexCompressNormalsOpt{"tex.compress.normals", false,
                                         "RX_TEX_COMPRESS_NORMALS"};

base::Option<bool> GpuTimings{"gpu.timings", false, "RX_GPU_TIMINGS"};

base::Option<int> MsaaOpt{"msaa", 0, "RX_MSAA"};

base::Option<bool> DrsOpt{"drs", false, "RX_DRS"};

base::Option<double> DrsTargetMs{"drs.target.ms", 16.6, "RX_DRS_TARGET_MS"};

base::Option<double> DrsMinScale{"drs.min.scale", 0.5, "RX_DRS_MIN_SCALE"};

base::Option<bool> VrsOpt{"vrs", true, "RX_VRS"};

base::Option<double> VrsThreshold{"vrs.threshold", 0.06, "RX_VRS_THRESHOLD"};

base::Option<bool> RestirDiOpt{"restir.di", false, "RX_RESTIR_DI"};

base::Option<bool> RcgiOpt{"rcgi", false, "RX_RCGI"};

// Force RCGI's world side through the software SDF tracer (no ray query).
// Implied on non-ray-query devices; on RT hardware this is the A/B toggle vs
// the TLAS path. Implies the SDF clipmap cost (RX_SDF is auto-enabled when RCGI
// needs it).
base::Option<bool> RcgiSwOpt{"rcgi.software", false, "RX_RCGI_SW"};

// SDF software-trace infrastructure (S1): mesh SDFs + global SDF clipmap. Off
// by default; RX_SDF_DEBUG raymarches the clipmap (1 = distance field, 2 =
// albedo).
base::Option<bool> SdfOpt{"sdf", false, "RX_SDF"};

base::Option<bool> FftOceanOpt{"fft.ocean", true, "RX_FFT_OCEAN"};

base::Option<bool> AdaptiveWaterOpt{"water.adaptive", true,
                                    "RX_ADAPTIVE_WATER"};

base::Option<bool> WaterFieldOpt{"water.field", true, "RX_WATER_FIELD"};

base::Option<bool> FluidSimOpt{"fluid.sim", false, "RX_FLUID_SIM"};

base::Option<bool> WaterInteractionOpt{"water.interaction", true,
                                       "RX_WATER_INTERACTION"};

base::Option<bool> ShoreWettingOpt{"shore.wetting", false, "RX_SHORE_WETTING"};

base::Option<bool> WaterCausticsOpt{"water.caustics", true,
                                    "RX_WATER_CAUSTICS"};

base::Option<bool> ProceduralGrassOpt{"procedural.grass", true,
                                      "RX_PROCEDURAL_GRASS"};

base::Option<double> HdrPaperWhite{"hdr.paper.white", 200.0,
                                   "RX_HDR_PAPER_WHITE"};

base::Option<bool> Wireframe{"wireframe", false, "RX_WIREFRAME"};

base::Option<bool> Ssr{"ssr", false, "RX_SSR"};

base::Option<bool> Ssgi{"ssgi", false, "RX_SSGI"};

base::Option<bool> DistanceLod{"distance.lod", false, "RX_DISTANCE_LOD"};

base::Option<bool> MeshShaderLod{"mesh.shader.lod", false,
                                 "RX_MESH_SHADER_LOD"};

base::Option<int> DebugViewOpt{"debug.view", 0, "RX_DEBUG_VIEW"};

base::Option<int> ColorGradeOpt{"color.grade", 0, "RX_COLOR_GRADE"};

base::Option<const char *> Lut{"lut", nullptr, "RX_LUT"};

base::Option<const char *> SunDir{"sun.dir", nullptr, "RX_SUN_DIR"};

base::Option<bool> Pathtrace{"pathtrace", false, "RX_PATHTRACE"};

base::Option<bool> PathtraceReference{"pathtrace.reference", false,
                                      "RX_PATHTRACE_REFERENCE"};

base::Option<int> PathtraceSpp{"pathtrace.spp", 2, "RX_PATHTRACE_SPP"};

base::Option<int> PathtraceAccum{"pathtrace.accum", 16, "RX_PATHTRACE_ACCUM"};

base::Option<bool> PathtraceRecon{"pathtrace.recon", false,
                                  "RX_PATHTRACE_RECON"};

base::Option<int> PathtraceReconDebug{"pathtrace.recon.debug", 0,
                                      "RX_PATHTRACE_RECON_DEBUG"};

base::Option<bool> PathtraceRestir{"pathtrace.restir", true,
                                   "RX_PATHTRACE_RESTIR"};

base::Option<bool> PathtraceRestirDi{"pathtrace.restir.di", true,
                                     "RX_PATHTRACE_RESTIR_DI"};

base::Option<bool> PathtraceRr{"pathtrace.rr", true, "RX_PATHTRACE_RR"};

base::Option<bool> Fog{"fog", false, "RX_FOG"};

base::Option<float> Aerial{"aerial", 1.0f, "RX_AERIAL"};

base::Option<bool> CloudsOpt{"clouds", false, "RX_CLOUDS"};

base::Option<bool> CloudscapeOpt{"cloudscape", false, "RX_CLOUDSCAPE"};

base::Option<float> CloudCoverage{"cloud.coverage", 0.46f, "RX_CLOUD_COVERAGE"};

base::Option<float> Precip{"precip", 0.0f, "RX_PRECIP"};

base::Option<bool> Snow{"snow", false, "RX_SNOW"};

base::Option<bool> Aurora{"aurora", false, "RX_AURORA"};

base::Option<float> Wind{"wind", 12.0f, "RX_WIND"};

base::Option<float> WindDir{"wind.dir", 0.0f, "RX_WIND_DIR"};

base::Option<float> Wetness{"wetness", 0.0f, "RX_WETNESS"};

base::Option<float> SnowCover{"snow.cover", 0.0f, "RX_SNOW_COVER"};

base::Option<float> AuroraIntensity{"aurora.intensity", 1.0f,
                                    "RX_AURORA_INTENSITY"};

base::Option<const char *> RhiBackend{"rhi.backend", nullptr, "RX_RHI"};

// Swapchain stand-in for a windowless renderer. It owns no presentable images
// (there is no surface); it only answers the size, format and color space the
// post pass and the frame graph are built against, so the rest of the renderer
// needs no windowless special case. A windowless frame always renders into
// capture_image_, so Acquire and image() are never reached. BGRA8 is what both
// backends negotiate for a real surface, and the byte order WriteBackbufferPng
// unswizzles on the way to a png.
class OffscreenSwapchain final : public gpu::Swapchain {
public:
  OffscreenSwapchain(gpu::Format format, gpu::Extent2D extent)
      : format_(format), extent_(extent) {}

  gpu::AcquireResult Acquire(u32, u32 *) override { return gpu::AcquireResult::kFailed; }
  gpu::Format format() const override { return format_; }
  gpu::Extent2D extent() const override { return extent_; }
  u32 image_count() const override { return 0; }
  const gpu::GpuImage &image(u32) const override {
    static const gpu::GpuImage kNone;
    return kNone;
  }
  bool can_sample() const override { return true; }

private:
  gpu::Format format_;
  gpu::Extent2D extent_;
};

} // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Initialize(const RendererDesc &desc, ui::Window &window) {
  return InitializeCommon(desc, &window, window.width(), window.height());
}

bool Renderer::InitializeOffscreen(const RendererDesc &desc, u32 width,
                                   u32 height) {
  offscreen_only_ = true;
  return InitializeCommon(desc, nullptr, width, height);
}

bool Renderer::InitializeCommon(const RendererDesc &desc, ui::Window *window,
                                u32 width, u32 height) {
  desc_ = desc;
  settings_.aa_mode = desc.aa_mode;
  settings_.upscaler = desc.upscaler;
  settings_.rt_shadows = desc.raytracing.shadows;
  output_width_ = width;
  output_height_ = height;
  // Applied before the swapchain exists (the rest of the option overrides run
  // later in Initialize; these two decide the surface format).
  if (HdrOutput.overridden())
    settings_.hdr_output = HdrOutput;
  if (HdrPaperWhite.overridden())
    settings_.hdr_paper_white = static_cast<f32>(double(HdrPaperWhite));

  // RX_RHI=vulkan|d3d12|null|auto overrides the graphics backend.
  gpu::Backend backend = desc.backend;
  if (const char *name = RhiBackend.get()) {
    base::String value = name;
    if (value == "vulkan")
      backend = gpu::Backend::kVulkan;
    else if (value == "d3d12")
      backend = gpu::Backend::kD3D12;
    else if (value == "null")
      backend = gpu::Backend::kNull;
    else if (value == "auto")
      backend = gpu::Backend::kAuto;
    else
      RX_WARN("RX_RHI: unknown backend '{}', using {}", value,
              gpu::BackendName(backend));
  }

  window_ = window;
  const gpu::DeviceDesc device_desc{
      .backend = backend,
      .enable_validation = desc.enable_validation,
      .request_raytracing = desc.enable_raytracing,
      .extra_device_extensions = desc.vulkan.extensions};
  device_ = window ? gpu::Device::Create(device_desc, *window)
                   : gpu::Device::CreateOffscreen(device_desc);
  if (device_->is_stub()) {
    RX_WARN("renderer running in stub mode");
    return true;
  }

  // Everything below creates pipelines; batch them so the driver compiles
  // across cores on a cold cache (the guard joins the workers on every exit
  // path, and a failed compile fails Initialize at the End check).
  struct PipelineBatchGuard {
    gpu::Device &device;
    bool ended = false;
    bool End() {
      ended = true;
      return device.EndPipelineBatch();
    }
    ~PipelineBatchGuard() {
      if (!ended)
        device.EndPipelineBatch();
    }
  } pipeline_batch{*device_};
  auto t_batch0 = base::TimeTicks::Now();
  const char *pso_batch_env = ::getenv("RX_PSO_BATCH");
  if (!pso_batch_env || pso_batch_env[0] != '0')
    device_->BeginPipelineBatch();

  swapchain_hdr_request_ = WantHdrSwapchain();
  if (settings_.hdr_output && !swapchain_hdr_request_) {
    RX_INFO("hdr output requested but the system is not compositing in hdr; "
            "using sdr "
            "(enable hdr in the os display settings)");
  }
  swapchain_ =
      window ? device_->CreateSwapchain(output_width_, output_height_,
                                        settings_.vsync, swapchain_hdr_request_)
             : base::MakeUnique<OffscreenSwapchain>(
                   gpu::Format::kBGRA8Unorm, gpu::Extent2D{output_width_, output_height_});
  if (!swapchain_ || !CreateFrameResources())
    return false;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;

  if (desc.enable_raytracing && device_->caps().raytracing) {
    raytracing_ = RayTracingContext::Create(*device_);
    if (raytracing_)
      raytracing_->Configure(desc.raytracing);
    else
      RX_WARN("ray tracing disabled: fallback tlas creation failed");
  }
  rt_available_ = raytracing_ && device_->caps().ray_query;
  // Skinned actors only reach the TLAS through this, so it lives and dies with
  // the ray-tracing context; without one Acquire hands back 0 and every draw
  // that asked for it simply stays out of ray tracing.
  if (raytracing_)
    skinned_rt_.Initialize(*device_);

  // Material textures compress at import, which needs a device that can sample
  // the block formats. This is the only place that knows both, and it runs
  // before any scene loads, so the loaders can read the flag without a device
  // handle of their own. RX_TEX_COMPRESS=0 forces the rgba8 path back for an
  // A/B against a suspected compression artifact.
  const bool compress_textures =
      device_->caps().texture_compression_bc && TexCompressOpt;
  asset::SetTextureCompression(
      {.supported = compress_textures, .normals = compress_textures && TexCompressNormalsOpt});
  if (!compress_textures) {
    RX_INFO("material texture compression off ({}); textures upload as rgba8",
            device_->caps().texture_compression_bc ? "RX_TEX_COMPRESS=0"
                                                   : "no textureCompressionBC");
  }

  transient_pool_ = base::MakeUnique<TransientPool>(*device_);
  // The bindless registry has no ray-tracing dependency (buffers + an
  // update-after-bind set): the forward terrain splat and textured particles
  // sample through it too, so it exists on every real device. Mesh/geometry
  // registration (device-address reads) stays gated on ray tracing below.
  bindless_ = BindlessRegistry::Create(*device_);
  if (!bindless_)
    return false;
  material_system_ = MaterialSystem::Create(*device_, bindless_.Get_UseOnlyIfYouKnowWhatYouareDoing());
  if (!material_system_)
    return false;
  environment_ = EnvironmentSystem::Create(*device_);
  if (!environment_)
    return false;
  mesh_pipeline_ = MeshPipeline::Create(
      *device_, kSceneColorFormat, kMotionFormat, kNormalFormat, kDepthFormat,
      material_system_->set_layout(), environment_->env_set_layout(),
      bindless_ ? bindless_->set_layout() : gpu::BindingLayoutHandle{});
  post_ = PostPass::Create(*device_, swapchain_->format());
  if (!mesh_pipeline_ || !post_ || !taa_.Initialize(*device_))
    return false;
  // kMsaa support: sample-0 guide resolve + the fullscreen depth rebuild that
  // hands the post-resolve raster passes a single-sampled depth buffer.
  msaa_resolve_pipeline_ = device_->CreateComputePipeline({
      .shader = RX_SHADER(k_msaa_resolve_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kSampledImage},
                          {1, gpu::BindingType::kSampledImage},
                          {2, gpu::BindingType::kStorageImage},
                          {3, gpu::BindingType::kStorageImage}}}},
      .push_constant_size = 8,
      .debug_name = "msaa_resolve",
  });
  depth_copy_pipeline_ = device_->CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_fullscreen_vs_slang),
      .fragment = RX_SHADER(k_depth_copy_ps_hlsl),
      .raster = {.cull = gpu::CullMode::kNone},
      .depth = {.test = true,
                .write = true,
                .compare = gpu::CompareOp::kAlways,
                .format = kDepthFormat},
      .sets = {{.slots = {{0, gpu::BindingType::kSampledImage}}}},
      .debug_name = "msaa_depth_copy",
  });
  hdr_overlay_copy_pipeline_ = device_->CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_fullscreen_vs_slang),
      .fragment = RX_SHADER(k_blit_ps_slang),
      .raster = {.cull = gpu::CullMode::kNone},
      .color_formats = {kSceneColorFormat},
      .blend = {gpu::BlendMode::kOpaque},
      .sets = {{.slots = {{0, gpu::BindingType::kCombinedTextureSampler}},
                .stages = gpu::kShaderStageFragment}},
      .debug_name = "hdr_overlay_copy",
  });
  hdr_overlay_sampler_ =
      device_->GetSampler({.address_u = gpu::AddressMode::kClampToEdge,
                           .address_v = gpu::AddressMode::kClampToEdge,
                           .address_w = gpu::AddressMode::kClampToEdge});
  if (!msaa_resolve_pipeline_ || !depth_copy_pipeline_ ||
      !hdr_overlay_copy_pipeline_ || !hdr_overlay_sampler_)
    return false;
  ui_blur_ = UiBlurPass::Create(*device_); // optional: frosted-glass UI blur
  if (rt_available_ && !rtao_.Initialize(*device_))
    return false;
  if (rt_available_ && bindless_ &&
      !reflection_trace_.Initialize(*device_, bindless_->set_layout())) {
    return false;
  }
  if (!motion_blur_.Initialize(*device_))
    return false;
  if (!dof_.Initialize(*device_))
    return false;
  {
    struct ClusterPush {
      Mat4 view;
      f32 screen[2];
      f32 near_plane;
      f32 slice_scale;
      f32 slice_bias;
      u32 light_count;
      f32 tan_half_fov_y;
      f32 aspect;
      u32 decal_count;
      f32 pad[3];
    };
    light_cluster_pipeline_ = device_->CreateComputePipeline({
        .shader = RX_SHADER(k_light_cluster_cs_hlsl),
        .sets = {{.slots = {{0, gpu::BindingType::kStorageBuffer},
                            {1, gpu::BindingType::kStorageBuffer},
                            {2, gpu::BindingType::kStorageBuffer},
                            {3, gpu::BindingType::kStorageBuffer},
                            {4, gpu::BindingType::kStorageBuffer}}}},
        .push_constant_size = gpu::PushSize<ClusterPush>(),
        .debug_name = "light_cluster",
    });
    if (!light_cluster_pipeline_)
      return false;
    // The two matrices alone are the whole 128 bytes vulkan guarantees for a
    // push block, so they ride in a per-frame uniform buffer (binding 2) and
    // the push keeps the scalars.
    struct ContactPush {
      f32 sun_dir[3];
      f32 near_plane;
      u32 size[2];
      f32 range;
      f32 thickness;
      u32 steps;
      u32 frame_index;
      f32 pad[2];
    };
    contact_shadow_pipeline_ = device_->CreateComputePipeline({
        .shader = RX_SHADER(k_contact_shadow_cs_hlsl),
        .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                            {1, gpu::BindingType::kSampledImage},
                            {2, gpu::BindingType::kUniformBuffer}}}},
        .push_constant_size = gpu::PushSize<ContactPush>(),
        .debug_name = "contact_shadow",
    });
    if (!contact_shadow_pipeline_)
      return false;
    struct CloudShadowPush {
      Mat4 inv_view_proj;
      f32 sun_dir[3];
      f32 near_plane;
      u32 size[2];
      f32 time;
      f32 coverage;
      f32 bottom;
      f32 top;
      f32 wind;
      f32 strength;
      f32 wind_z; // z drift velocity, matches cloud_shadow.cs (and clouds.cs)
      f32 pad[3];
    };
    cloud_shadow_pipeline_ = device_->CreateComputePipeline({
        .shader = RX_SHADER(k_cloud_shadow_cs_hlsl),
        .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                            {1, gpu::BindingType::kSampledImage}}}},
        .push_constant_size = gpu::PushSize<CloudShadowPush>(),
        .debug_name = "cloud_shadow",
    });
    if (!cloud_shadow_pipeline_)
      return false;
    struct SssPush {
      u32 size[2];
      f32 inv_size[2];
      f32 dir[2];
      f32 near_plane;
      f32 width;
      f32 proj_scale;
      f32 max_radius;
      u32 composite;
      f32 strength;
    };
    sss_pipeline_ = device_->CreateComputePipeline({
        .shader = RX_SHADER(k_sss_blur_cs_hlsl),
        .sets = {{.slots = {{0, gpu::BindingType::kStorageImage},
                            {1, gpu::BindingType::kCombinedTextureSampler},
                            {2, gpu::BindingType::kCombinedTextureSampler},
                            {3, gpu::BindingType::kCombinedTextureSampler}}}},
        .push_constant_size = gpu::PushSize<SssPush>(),
        .debug_name = "sss_blur",
    });
    if (!sss_pipeline_)
      return false;
    sss_sampler_ =
        device_->GetSampler({.address_u = gpu::AddressMode::kClampToEdge,
                             .address_v = gpu::AddressMode::kClampToEdge});
    cluster_counts_ =
        device_->CreateBuffer(kClusterCount * sizeof(u32), gpu::kBufferUsageStorage);
    cluster_indices_ = device_->CreateBuffer(
        static_cast<u64>(kClusterCount) * kMaxLightsPerCluster * sizeof(u32),
        gpu::kBufferUsageStorage);
    decal_cluster_indices_ = device_->CreateBuffer(
        static_cast<u64>(kClusterCount) * kMaxDecalsPerCluster * sizeof(u32),
        gpu::kBufferUsageStorage);
    if (!cluster_counts_ || !cluster_indices_ || !decal_cluster_indices_)
      return false;
    // One per in-flight frame: the contact-shadow pass rewrites it while the
    // previous frame may still be reading its own copy.
    for (gpu::GpuBuffer &camera : contact_camera_) {
      camera = device_->CreateBuffer(sizeof(internal::ContactCamera), gpu::kBufferUsageUniform,
                                     true);
      if (!camera.mapped)
        return false;
    }
  }
  if (!ssao_.Initialize(*device_))
    return false; // raster ao fallback, no rt needed
  if (!ssr_.Initialize(*device_))
    return false; // raster reflection fallback
  if (!ssgi_.Initialize(*device_))
    return false; // raster diffuse-gi fallback

  // Persistent per-slot sets for the frame globals and environment bindings.
  // Contents are rewritten each frame after the slot's fence has fired, before
  // any pass of the new frame binds them.
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    globals_sets_[i] = device_->CreateBindingSet(mesh_pipeline_->set_layout());
    env_scene_sets_[i] =
        device_->CreateBindingSet(environment_->env_set_layout());
    env_transparent_sets_[i] =
        device_->CreateBindingSet(environment_->env_set_layout());
    env_prepass_sets_[i] =
        device_->CreateBindingSet(environment_->env_set_layout());
    if (!globals_sets_[i] || !env_scene_sets_[i] || !env_transparent_sets_[i] ||
        !env_prepass_sets_[i]) {
      return false;
    }
  }

  // Linear-hdr export: a compute copy from the resolved scene into a host
  // buffer.
// Width and height of the captured image. Named so its size goes through
  // the same guard as every other push block.
  struct HdrCapturePush {
    u32 width;
    u32 height;
  };
  hdr_pipeline_ = device_->CreateComputePipeline({
      .shader = RX_SHADER(k_hdr_capture_cs_hlsl),
      .sets = {{.slots = {{0, gpu::BindingType::kStorageBuffer},
                          {1, gpu::BindingType::kSampledImage}}}},
      .push_constant_size = gpu::PushSize<HdrCapturePush>(),
      .debug_name = "hdr_capture",
  });
  if (!hdr_pipeline_)
    return false;

  if (!local_shadows_.Initialize(*device_))
    return false; // clustered light shadows
  if (!shadow_.Initialize(*device_, material_system_->set_layout(),
                          local_shadows_.atlas().format))
    return false; // raster sun and local-shadow pipelines
  if (!froxel_fog_.Initialize(*device_, rt_available_)) {
    RX_WARN("froxel volumetrics unavailable"); // non-fatal: feature gates on
                                               // available()
  }
  vrs_.Initialize(*device_); // non-fatal: needs attachment VRS hardware
  if (rt_available_)
    restir_di_.Initialize(*device_);     // non-fatal: gates on available()
  virtual_texture_.Initialize(*device_); // non-fatal: gates on available()
  decal_baker_.Initialize(*device_);     // non-fatal: gates on available()
  if (!particles_.Initialize(*device_, kSceneColorFormat,
                             bindless_ ? bindless_->set_layout()
                                       : gpu::BindingLayoutHandle{}))
    return false;
  if (!gaussians_.Initialize(*device_, kSceneColorFormat))
    return false;
  if (!fur_.Initialize(*device_, kSceneColorFormat, kDepthFormat))
    return false;
  if (!wboit_.Initialize(*device_, kSceneColorFormat, kDepthFormat))
    return false;
  if (!overdraw_.Initialize(*device_, kSceneColorFormat))
    return false;
  if (!gpu_cull_.Initialize(*device_, kSceneColorFormat))
    return false;
  if (!meshlet_.Initialize(*device_, kSceneColorFormat, kDepthFormat))
    return false;
  if (!vgeo_.Initialize(*device_, kSceneColorFormat, kDepthFormat))
    return false;
  if (!hair_.Initialize(*device_, kSceneColorFormat, kDepthFormat))
    return false;
  if (!imposters_.Initialize(*device_, kSceneColorFormat, kDepthFormat))
    return false;
  if (!ocean_.Initialize(*device_)) {
    RX_WARN("fft ocean unavailable"); // non-fatal: gerstner fallback
  }
  if (!water_field_.Initialize(*device_)) {
    RX_WARN(
        "water foam field unavailable"); // non-fatal: instantaneous crest foam
  }
  if (!fluid_sim_.Initialize(*device_)) {
    RX_WARN("fluid sim unavailable"); // non-fatal: optional feature, off by default
  }
  if (!shore_wetting_.Initialize(*device_)) {
    RX_WARN("shoreline wetting unavailable"); // non-fatal: feature stays off
  }
  if (!water_caustics_.Initialize(*device_)) {
    RX_WARN("water caustics unavailable"); // non-fatal: no seafloor caustics
  }
  if (device_->caps().mesh_shaders) {
    // 1x1 fallback hi-z so the mesh-shader cull descriptor is always valid;
    // bound (with occlusion disabled) on frames where no real hi-z was built.
    ms_dummy_hiz_ =
        device_->CreateImage2D(gpu::Format::kR32Float, {1, 1}, gpu::kTextureUsageSampled);
    device_->ImmediateSubmit([&](gpu::CommandList &cmd) {
      cmd.Barrier(gpu::Transition(ms_dummy_hiz_, gpu::ResourceState::kUndefined,
                             gpu::ResourceState::kShaderReadAll));
    });
  }
  if (!reference_compare_.Initialize(*device_))
    RX_WARN("reference compare unavailable; --demo lookdev comparison disabled");
  if (!bloom_.Initialize(*device_) || !exposure_.Initialize(*device_))
    return false;
  if (rt_available_) {
    ddgi_ = DdgiSystem::Create(*device_, environment_->sky_view(),
                               environment_->sampler(), *bindless_);
    if (!ddgi_)
      return false;
    // RCGI (idTech8-style radiance-cached GI) is ~85 MiB and off by default, so
    // it is created lazily on first activation (ApplySettings), not here. Its
    // software SDF path also makes it available on non-ray-query devices.
    water_ = WaterPass::Create(
        *device_, kSceneColorFormat, kMotionFormat, kDepthFormat,
        mesh_pipeline_->set_layout(), material_system_->set_layout(),
        environment_->env_set_layout(), bindless_->set_layout());
    if (!water_)
      return false;
  }
  // Fluid surface renderer: independent of ray tracing (the sim runs on any
  // device), so it is created outside the rt gate. Non-fatal: the optional
  // solver simply draws nothing if this fails.
  fluid_surface_ = FluidSurfacePass::Create(
      *device_, kSceneColorFormat, kMotionFormat, kDepthFormat,
      mesh_pipeline_->set_layout(), environment_->env_set_layout(),
      bindless_ ? bindless_->set_layout() : gpu::BindingLayoutHandle{});
  if (!fluid_surface_)
    RX_WARN("fluid surface renderer unavailable"); // optional feature stays off
  if (!environment_->CreateSkyPipeline(mesh_pipeline_->set_layout(),
                                       kSceneColorFormat, kMotionFormat,
                                       kDepthFormat)) {
    return false;
  }

  if (settings_.upscaler != UpscalerKind::kNone &&
      !CreateUpscalerWithFallback()) {
    RX_WARN("upscaler unavailable, falling back to taa");
    settings_.upscaler = UpscalerKind::kNone;
    settings_.aa_mode = AntiAliasingMode::kTaa;
  }
  applied_upscaler_ = settings_.upscaler;
  applied_quality_ = settings_.upscaler_quality;
  applied_aa_ = settings_.aa_mode;
  applied_vsync_ = settings_.vsync;

  profiler_.Initialize(*device_, kFramesInFlight);
  if (rt_available_ && bindless_) {
    path_tracer_.Initialize(*device_, bindless_->set_layout());
    recon_path_tracer_.Initialize(*device_, bindless_->set_layout());
  }
  if (rt_available_)
    volumetric_fog_.Initialize(*device_);
  aerial_perspective_.Initialize(
      *device_);                // atmospheric distance haze (no ray tracing)
  clouds_.Initialize(*device_); // volumetric clouds (no ray tracing)
  precipitation_.Initialize(*device_);   // screen-space rain/snow
  surface_weather_.Initialize(*device_); // rain wetness / snow accumulation
  if (!precip_occlusion_.Initialize(*device_)) {
    RX_WARN("precipitation sky occlusion unavailable"); // volumetric precip
                                                        // gates on it
  }
  if (!precip_volume_.Initialize(*device_, kSceneColorFormat, rt_available_)) {
    RX_WARN(
        "volumetric precipitation unavailable"); // screen-space streaks remain
  }
  if (!lightning_.Initialize(*device_, kSceneColorFormat)) {
    RX_WARN("lightning bolts unavailable"); // the global flash scalar remains
  }

  UpdateRenderResolution();
  vrs_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_)
    restir_di_.Resize(*device_, {render_width_, render_height_});
  taa_.Resize(*device_, {render_width_, render_height_});
  ssao_.Resize(*device_, {render_width_, render_height_});
  ssr_.Resize(*device_, {render_width_, render_height_});
  ssgi_.Resize(*device_, {render_width_, render_height_});
  path_tracer_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_ && settings_.path_trace_recon) {
    recon_path_tracer_.Resize(*device_, {render_width_, render_height_});
  }
  if (rt_available_)
    rtao_.Resize(*device_, {render_width_, render_height_});
#if defined(RX_HAS_NRD)
  if (rt_available_ &&
      !nrd_.Initialize(*device_, {render_width_, render_height_})) {
    RX_WARN("nrd denoiser unavailable, rtao/shadow denoising disabled");
  }
  if (rt_available_ && !shadow_trace_.Initialize(*device_)) {
    RX_WARN("shadow trace unavailable, sigma sun-shadow denoising disabled");
  }
  if (rt_available_)
    shadow_trace_.Resize(*device_, {render_width_, render_height_});
#endif

  // Debug captures without window manager screenshots:
  // RX_SCREENSHOT=/tmp/frame.png:12 saves the frame at t=12s.
  if (const char *spec = Screenshot.get()) {
    base::String value = spec;
    size_t colon = value.find_last_of(':');
    if (colon != base::String::npos) {
      screenshot_at_ = ::atof(value.c_str() + colon + 1);
      value.resize(colon);
    }
    screenshot_path_ = value;
  }

  // RX_SEQ=prefix:startsec:count[:stride] dumps a burst of composited frames.
  if (const char *spec = Sequence.get()) {
    base::String value = spec;
    base::Vector<base::String> fields;
    size_t start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
      if (i == value.size() || value[i] == ':') {
        fields.push_back(value.substr(start, i - start));
        start = i + 1;
      }
    }
    if (fields.size() >= 3) {
      seq_prefix_ = fields[0];
      seq_at_ = ::atof(fields[1].c_str());
      seq_count_ = ::atoi(fields[2].c_str());
      seq_stride_ =
          fields.size() >= 4 ? rx::Max(1, ::atoi(fields[3].c_str())) : 1;
    } else {
      RX_WARN(
          "RX_SEQ ignored, expected prefix:startsec:count[:stride], got '{}'",
          spec);
    }
  }

  // RX_HDR=/tmp/frame.hdr:12 exports the linear-hdr frame (radiance rgbe) at
  // t=12s.
  if (const char *spec = Hdr.get()) {
    base::String value = spec;
    size_t colon = value.find_last_of(':');
    if (colon != base::String::npos) {
      hdr_at_ = ::atof(value.c_str() + colon + 1);
      value.resize(colon);
    }
    hdr_path_ = value;
  }

  if (Wireframe.overridden())
    settings_.wireframe = Wireframe;
  if (Ssr.overridden())
    settings_.ssr = Ssr;
  if (Ssgi.overridden())
    settings_.ssgi = Ssgi;
  // RX_DISTANCE_LOD=1 re-enables distance-based lod downgrade (off by default;
  // the engine otherwise always renders the finest authored detail).
  if (DistanceLod.overridden())
    settings_.distance_lod = DistanceLod;
  // RX_MESH_SHADER_LOD=1 opts into the optional mesh-shader opaque path.
  if (MeshShaderLod.overridden())
    settings_.mesh_shader_lod = MeshShaderLod;
  // Hardware gate: the path needs mesh shaders and its pipelines to have
  // built. Disable + warn rather than silently doing nothing if it was
  // requested.
  bool mesh_shader_ok = device_->caps().mesh_shaders && mesh_pipeline_ &&
                        mesh_pipeline_->has_mesh_shader();
  if (mesh_shader_ok) {
    RX_INFO("mesh-shader lod path available (default {})",
            settings_.mesh_shader_lod ? "on" : "off");
  } else {
    if (settings_.mesh_shader_lod) {
      RX_WARN(
          "mesh-shader lod requested but unavailable on this gpu, disabling");
    }
    settings_.mesh_shader_lod = false;
  }

  // RX_DEBUG_VIEW=<n> pins a debug channel at startup for headless capture;
  // exposure is fixed so the channel reads at its true magnitude.
  if (DebugViewOpt.overridden()) {
    settings_.debug_view = static_cast<DebugView>(DebugViewOpt.get());
    if (settings_.debug_view != DebugView::kOff) {
      settings_.auto_exposure = false;
      settings_.exposure = 1.0f;
    }
  }
  if (ColorGradeOpt.overridden()) {
    settings_.color_grade = static_cast<ColorGrade>(ColorGradeOpt.get());
  }
  // RX_LUT=<path> loads an external .cube 3D lut as the active color grade.
  if (const char *lut = Lut.get()) {
    if (post_ && post_->LoadCubeLut(lut))
      settings_.color_grade = ColorGrade::kCustom;
  }
  // RX_SUN_DIR="x,y,z" overrides the sun travel direction, for headless
  // lighting/shadow tests (normalized; y clamped below the horizon).
  if (const char *sd = SunDir.get()) {
    Vec3 d{};
    if (::sscanf(sd, "%f,%f,%f", &d.x, &d.y, &d.z) == 3) {
      f32 len = ::sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
      if (len > 1e-4f)
        settings_.sun_direction = {d.x / len, d.y / len, d.z / len};
    }
  }
  if (Pathtrace.overridden())
    settings_.path_trace = Pathtrace;
  if (PathtraceReference.overridden())
    settings_.path_trace_reference = PathtraceReference;
  if (MotionBlurOpt.overridden())
    settings_.motion_blur = MotionBlurOpt;
  if (DofOpt.overridden())
    settings_.dof = DofOpt;
  if (LensFlareOpt.overridden())
    settings_.lens_flare = static_cast<f32>(double(LensFlareOpt));
  if (GrainOpt.overridden())
    settings_.film_grain = static_cast<f32>(double(GrainOpt));
  if (DofFocus.overridden())
    settings_.dof_focus = static_cast<f32>(double(DofFocus));
  if (DofAperture.overridden())
    settings_.dof_aperture = static_cast<f32>(double(DofAperture));
  if (SssOpt.overridden())
    settings_.sss = SssOpt;
  if (SssWidth.overridden())
    settings_.sss_width = static_cast<f32>(double(SssWidth));
  if (SkinDynamicsOpt.overridden())
    settings_.skin_dynamics = SkinDynamicsOpt;
  if (SkinHeartRateOpt.overridden())
    settings_.skin_heart_rate = static_cast<f32>(double(SkinHeartRateOpt));
  if (SkinPerfusionOpt.overridden())
    settings_.skin_perfusion = static_cast<f32>(double(SkinPerfusionOpt));
  if (SkinPulseAmpOpt.overridden())
    settings_.skin_pulse_amplitude = static_cast<f32>(double(SkinPulseAmpOpt));
  if (SkinTensionGainOpt.overridden())
    settings_.skin_tension_gain = static_cast<f32>(double(SkinTensionGainOpt));
  if (AsyncComputeOpt.overridden())
    settings_.async_compute = AsyncComputeOpt;
  if (FrameGenOpt.overridden())
    settings_.frame_generation = FrameGenOpt;
  if (LocalShadowsOpt.overridden())
    settings_.local_shadows = LocalShadowsOpt;
  if (FroxelOpt.overridden())
    settings_.froxel_fog = FroxelOpt;
  if (FroxelDensity.overridden())
    settings_.froxel_density = static_cast<f32>(double(FroxelDensity));
  if (FroxelStartDistance.overridden())
    settings_.froxel_start_distance = static_cast<f32>(double(FroxelStartDistance));
  // Tracked separately: overriding one must not suppress the other's authored
  // value, which would silently zero a stage's fog start.
  froxel_density_overridden_ = FroxelDensity.overridden();
  froxel_start_overridden_ = FroxelStartDistance.overridden();
  // RX_TEX_BUDGET_MB caps resident material-texture memory (mip streaming);
  // -1 auto (half of vram), 0 unlimited.
  if (TexBudgetMb.overridden())
    settings_.texture_budget_mb = TexBudgetMb;
  // RX_GPU_TIMINGS forces per-pass timestamps on for headless profiling.
  if (GpuTimings.overridden())
    settings_.gpu_pass_timings = GpuTimings;
  // RX_MSAA=2/4/8 selects the hardware-MSAA AA mode (no temporal component);
  // 0/1 leaves the configured mode alone.
  if (MsaaOpt.overridden() && static_cast<int>(MsaaOpt) >= 2) {
    settings_.aa_mode = AntiAliasingMode::kMsaa;
    settings_.msaa_samples = static_cast<u32>(static_cast<int>(MsaaOpt));
  }
  // RX_DRS holds the GPU frame time at RX_DRS_TARGET_MS by stepping the
  // render scale, no lower than RX_DRS_MIN_SCALE per axis.
  if (DrsOpt.overridden())
    settings_.dynamic_resolution = DrsOpt;
  if (DrsTargetMs.overridden())
    settings_.dynamic_target_ms = static_cast<f32>(double(DrsTargetMs));
  if (DrsMinScale.overridden())
    settings_.dynamic_min_scale = static_cast<f32>(double(DrsMinScale));
  if (VrsOpt.overridden())
    settings_.vrs = VrsOpt;
  if (VrsThreshold.overridden())
    settings_.vrs_threshold = static_cast<f32>(double(VrsThreshold));
  if (RestirDiOpt.overridden())
    settings_.restir_di = RestirDiOpt;
  if (RcgiOpt.overridden())
    settings_.rcgi = RcgiOpt;
  rcgi_env_overridden_ = RcgiOpt.overridden();
  if (FftOceanOpt.overridden())
    settings_.fft_ocean = FftOceanOpt;
  if (AdaptiveWaterOpt.overridden())
    settings_.adaptive_water = AdaptiveWaterOpt;
  if (WaterFieldOpt.overridden())
    settings_.water_field = WaterFieldOpt;
  if (FluidSimOpt.overridden())
    settings_.fluid_sim = FluidSimOpt;
  if (WaterInteractionOpt.overridden())
    settings_.water_interaction = WaterInteractionOpt;
  if (ShoreWettingOpt.overridden())
    settings_.shore_wetting = ShoreWettingOpt;
  if (WaterCausticsOpt.overridden())
    settings_.water_caustics = WaterCausticsOpt;
  if (ProceduralGrassOpt.overridden())
    settings_.procedural_grass = ProceduralGrassOpt;
  if (PathtraceSpp.overridden())
    settings_.path_trace_spp = static_cast<u32>(rx::Max(1, int(PathtraceSpp)));
  if (PathtraceAccum.overridden())
    settings_.path_trace_accum =
        static_cast<u32>(rx::Max(1, int(PathtraceAccum)));
  if (PathtraceRecon.overridden())
    settings_.path_trace_recon = PathtraceRecon;
  if (PathtraceReconDebug.overridden())
    settings_.path_trace_recon_debug =
        static_cast<u32>(rx::Max(0, int(PathtraceReconDebug)));
  if (PathtraceRestir.overridden())
    settings_.path_trace_restir = PathtraceRestir;
  if (PathtraceRestirDi.overridden())
    settings_.path_trace_restir_di = PathtraceRestirDi;
  if (PathtraceRr.overridden())
    settings_.path_trace_rr = PathtraceRr;
  if (Fog.overridden())
    settings_.fog = Fog;
  // RX_AERIAL overrides aerial-perspective strength (0 off, 1 physical, >1
  // exaggerated).
  if (Aerial.overridden())
    settings_.aerial_perspective = Aerial.get();
  if (CloudsOpt.overridden())
    settings_.clouds = CloudsOpt;
  if (CloudscapeOpt.overridden())
    settings_.cloudscape = CloudscapeOpt;
  if (CloudCoverage.overridden())
    settings_.cloud_coverage = CloudCoverage.get();
  // RX_PRECIP forces precipitation (0..1) and RX_SNOW=1 makes it snow, so the
  // effect is testable without a loaded game's weather.
  if (Precip.overridden())
    settings_.weather.precipitation = Precip.get();
  if (Snow.overridden())
    settings_.weather.snow = Snow;
  if (Aurora.overridden())
    settings_.weather.aurora = Aurora;
  // RX_WIND (m/s) / RX_WIND_DIR (degrees) steer precipitation slant and cloud
  // drift; RX_WETNESS / RX_SNOW_COVER force the surface response directly.
  if (Wind.overridden())
    settings_.weather.wind_speed = Wind.get();
  if (WindDir.overridden())
    settings_.weather.wind_yaw = WindDir.get() * 3.14159265f / 180.0f;
  if (Wetness.overridden())
    settings_.weather.wetness = Wetness.get();
  if (SnowCover.overridden())
    settings_.weather.snow_cover = SnowCover.get();
  if (AuroraIntensity.overridden())
    settings_.weather.aurora_intensity = AuroraIntensity.get();

  // RCGI software mode: on a device without ray query (or RX_RCGI_SW on RT
  // hardware for A/B) RCGI's world side runs through the SDF clipmap tracer.
  // SDF availability is an IMMUTABLE STARTUP decision, not a live settings bit:
  // the CPU mesh data used to voxelise is not retained after upload, so a field
  // cannot be backfilled on a later toggle, and a live quality preset must not
  // turn a seeded path off. Decide want_sdf once here (startup desc flag, the
  // SDF-implying envs RX_SDF / RX_RCGI_SW, or a non-RT RCGI request) and gate
  // it on creation success into sdf_available_; it stays fixed for the session,
  // so RCGI-software carries the SDF memory + compose cost throughout. A late
  // programmatic rcgi enable on a non-RT device gets no software path;
  // ApplySettings logs that once.
  if (RcgiSwOpt.overridden())
    rcgi_force_software_ = RcgiSwOpt;
  const bool want_sdf = desc.software_gi || (desc.software_gi_fallback && !rt_available_) ||
                        SdfOpt.get() ||
                        rcgi_force_software_ ||
                        (settings_.rcgi && !rt_available_);

  // RCGI is created lazily on first activation (ApplySettings), covering both
  // the RT (hw + sw pipelines, so RX_RCGI_SW A/B works) and the non-ray-query
  // software-only path once the SDF clipmap is up. The SDF infrastructure below
  // is what makes the software path possible, but the ~85 MiB RCGI cache itself
  // is deferred so a device that never enables it pays nothing.

  auto t_batch1 = base::TimeTicks::Now();
  if (!pipeline_batch.End()) {
    RX_ERROR("pipeline batch reported failed compilations");
    return false;
  }
  auto t_batch2 = base::TimeTicks::Now();
  RX_INFO(
      "renderer init: {} ms (pipeline batch joined in {} ms)",
      (t_batch1 - t_batch0).InMilliseconds(),
      (t_batch2 - t_batch1).InMilliseconds());

  // Grass is optional and creates nonstandard push-constant layouts. Build its
  // baseline pipelines outside the startup batch so a device that cannot
  // support them degrades cleanly instead of invalidating every batched
  // pipeline. MSAA variants are created lazily when their sample count is used.
  if (!procedural_grass_.Initialize(*device_, kSceneColorFormat, kMotionFormat,
                                    kNormalFormat,
                                    MeshPipeline::kSkinDiffuseFormat,
                                    kDepthFormat)) {
    RX_WARN("procedural grass unavailable");
    procedural_grass_.Destroy(*device_);
  }

  // SDF software-trace infrastructure (RX_SDF) is created AFTER the pipeline
  // batch is joined, on purpose: it is an OPTIONAL, non-fatal path, but inside
  // a batch Create*Pipeline returns a placeholder handle that only fails at
  // EndPipelineBatch; a failed SDF pipeline would then abort the whole
  // renderer above instead of degrading. Built immediately here, a pipeline (or
  // 3D-storage -image) failure surfaces at this call site and is handled
  // non-fatally: log, tear the SDF systems down, leave the software path
  // unavailable. (RcgiSystem's _sw pipelines are likewise created outside any
  // batch (lazily in ApplySettings during RenderFrame), so a failure there
  // returns a null system and is handled non-fatally at that call site too.)
  if (want_sdf) {
    sdf_scene_ = base::MakeUnique<SdfScene>(*device_);
    sdf_clipmap_ = base::MakeUnique<SdfClipmap>(*device_);
    if (!sdf_clipmap_->Initialize()) {
      RX_WARN("sdf: clipmap unavailable, disabling the SDF path");
      sdf_clipmap_.Reset();
      sdf_scene_.Reset();
    } else {
      sdf_available_ = true; // immutable for the session once creation succeeds
    }
  }
  return true;
}

bool Renderer::SetEnvironmentMap(const f32 *rgba, u32 width, u32 height,
                                 const Vec3 &tint, f32 intensity,
                                 f32 rotation_radians) {
  if (!environment_) return false;
  // The cubemap is only re-convolved when the sun moves; an authored dome does
  // not move, so nudge the cached sun so the next frame rebuilds it.
  applied_sun_intensity_ = -1.0f;
  env_baked_sun_intensity_ = -1.0f;
  return environment_->SetEnvironmentMap(rgba, width, height, tint, intensity,
                                         rotation_radians);
}

void Renderer::ClearEnvironmentMap() {
  if (!environment_) return;
  applied_sun_intensity_ = -1.0f;
  env_baked_sun_intensity_ = -1.0f;
  environment_->ClearEnvironmentMap();
}

bool Renderer::CreateUpscalerForSettings() {
  f32 scale = UpscalerScale(settings_.upscaler_quality);
  u32 render_width = static_cast<u32>(static_cast<f32>(output_width_) / scale);
  u32 render_height =
      static_cast<u32>(static_cast<f32>(output_height_) / scale);
  upscaler_ = CreateUpscaler({.kind = settings_.upscaler,
                              .render_width = render_width,
                              .render_height = render_height,
                              .output_width = output_width_,
                              .output_height = output_height_,
                              .sharpness = settings_.sharpness},
                             *device_);
  if (upscaler_) {
    settings_.aa_mode = AntiAliasingMode::kUpscaler;
    return true;
  }
  return false;
}

bool Renderer::CreateUpscalerWithFallback() {
  if (CreateUpscalerForSettings())
    return true;
  // The preset picks dlss for any nvidia adapter and xess for intel, but both
  // need a vendor runtime the machine may simply not have (a broken or absent
  // ngx install is the common one, and it fails at context creation, not at
  // load). Fsr3 is vendor-agnostic and runs on any vulkan device, so it is the
  // second choice rather than dropping straight to taa. Without this an nvidia
  // box with no working ngx renders every frame at native resolution and pays
  // close to twice the frame cost, with nothing in the log but "falling back to
  // taa" to say why.
  if (settings_.upscaler == UpscalerKind::kFsr3)
    return false;
  const UpscalerKind requested = settings_.upscaler;
  settings_.upscaler = UpscalerKind::kFsr3;
  if (CreateUpscalerForSettings()) {
    RX_WARN("{} upscaler unavailable, using fsr3 instead",
            UpscalerName(requested));
    return true;
  }
  // Leave the request in place so the caller's warning names what was asked
  // for, not the substitute that also failed.
  settings_.upscaler = requested;
  return false;
}

void Renderer::UpdateRenderResolution() {
  if (upscaler_ && settings_.aa_mode == AntiAliasingMode::kUpscaler) {
    f32 scale = UpscalerScale(settings_.upscaler_quality);
    render_width_ = static_cast<u32>(static_cast<f32>(output_width_) / scale);
    render_height_ = static_cast<u32>(static_cast<f32>(output_height_) / scale);
  } else {
    // No upscaler: render at output * render_scale. >1 supersamples (the post
    // pass samples this image into the swapchain, so it downscales for free).
    // Dynamic resolution multiplies in as a <=1 factor while active.
    f32 rs = rx::Clamp(settings_.render_scale * drs_.scale(), 0.25f, 2.0f);
    render_width_ =
        rx::Max(1u, static_cast<u32>(static_cast<f32>(output_width_) * rs));
    render_height_ =
        rx::Max(1u, static_cast<u32>(static_cast<f32>(output_height_) * rs));
  }
}

void Renderer::ResizeSizedPasses() {
  vrs_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_)
    restir_di_.Resize(*device_, {render_width_, render_height_});
  taa_.Resize(*device_, {render_width_, render_height_});
  ssao_.Resize(*device_, {render_width_, render_height_});
  ssr_.Resize(*device_, {render_width_, render_height_});
  ssgi_.Resize(*device_, {render_width_, render_height_});
  path_tracer_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_ && settings_.path_trace_recon) {
    recon_path_tracer_.Resize(*device_, {render_width_, render_height_});
  }
  if (rt_available_)
    rtao_.Resize(*device_, {render_width_, render_height_});
#if defined(RX_HAS_NRD)
  if (rt_available_ && nrd_.available())
    nrd_.Resize(*device_, {render_width_, render_height_});
  if (rt_available_)
    shadow_trace_.Resize(*device_, {render_width_, render_height_});
#endif
#if defined(RX_HAS_DLSS)
  rr_.Resize(*device_, {render_width_, render_height_});
#endif
}

void Renderer::ApplySettings() {
  if (settings_.vsync != applied_vsync_) {
    applied_vsync_ = settings_.vsync;
    RecreateSwapchain();
  }

  // Create the opt-in cloud resources before BeginFrame: initialization does
  // one immediate transition submission and must not stall inside frame graph
  // recording. Keep the static bakes warm across toggles, but release the
  // resolution-dependent history allocation while disabled.
  const bool want_cloudscape =
      settings_.cloudscape && !settings_.interior &&
      !(settings_.path_trace && rt_available_ && bindless_ != nullptr);
  if (want_cloudscape && !cloudscape_init_tried_) {
    cloudscape_init_tried_ = true;
    cloudscape_ready_ = cloudscape_.Initialize(*device_);
    if (!cloudscape_ready_)
      RX_ERROR("cloudscape init failed, keeping procedural clouds");
  }
  if (!want_cloudscape && applied_cloudscape_ && cloudscape_ready_)
    cloudscape_.ReleaseHistory(*device_);
  applied_cloudscape_ = want_cloudscape;

  // kUpscaler is only valid with a live upscaler.
  if (settings_.aa_mode == AntiAliasingMode::kUpscaler &&
      settings_.upscaler == UpscalerKind::kNone) {
    settings_.aa_mode = AntiAliasingMode::kTaa;
  }

  // MSAA is a raster-geometry mode: the path tracer bypasses the raster path
  // entirely and its water prepass would bind multisampled prepass pipelines
  // on single-sampled targets, so path tracing wins while both are asked for.
  if (settings_.aa_mode == AntiAliasingMode::kMsaa && settings_.path_trace) {
    settings_.aa_mode = AntiAliasingMode::kTaa;
  }
  // The sample count bakes into the mesh pipelines; a mode/count change
  // rebuilds them through a device idle, like an upscaler swap.
  u32 want_msaa = 1;
  if (settings_.aa_mode == AntiAliasingMode::kMsaa) {
    want_msaa = settings_.msaa_samples >= 8   ? 8u
                : settings_.msaa_samples >= 4 ? 4u
                                              : 2u;
  }
  if (want_msaa != applied_msaa_samples_ && mesh_pipeline_) {
    device_->WaitIdle();
    auto rebuilt = MeshPipeline::Create(
        *device_, kSceneColorFormat, kMotionFormat, kNormalFormat, kDepthFormat,
        material_system_->set_layout(), environment_->env_set_layout(),
        bindless_ ? bindless_->set_layout() : gpu::BindingLayoutHandle{}, want_msaa);
    if (rebuilt) {
      mesh_pipeline_ = base::move(rebuilt);
      applied_msaa_samples_ = want_msaa;
      RX_INFO("msaa: mesh pipelines rebuilt at {}x", want_msaa);
    } else {
      RX_WARN("msaa: mesh pipeline rebuild failed, keeping {}x",
              applied_msaa_samples_);
      settings_.aa_mode = applied_msaa_samples_ > 1 ? AntiAliasingMode::kMsaa
                                                    : AntiAliasingMode::kTaa;
    }
    transient_pool_->Clear();
    taa_.Reset();
    has_prev_frame_ = false;
  }

  if (material_system_) {
    u64 budget = settings_.texture_budget_mb < 0
                     ? device_->caps().device_local_bytes / 2
                     : static_cast<u64>(settings_.texture_budget_mb) << 20;
    material_system_->SetBudget(budget);
  }

  // Dynamic resolution: stepped controller on the resolved GPU frame time.
  // Inert while a vendor upscaler pins the render ratio, or while the path
  // tracer runs (a step resets its accumulation every time it fires).
  bool drs_active =
      settings_.dynamic_resolution && !settings_.path_trace &&
      !(upscaler_ && settings_.aa_mode == AntiAliasingMode::kUpscaler);
  if (drs_active) {
    drs_.Configure({.target_ms = settings_.dynamic_target_ms,
                    .min_scale = settings_.dynamic_min_scale});
    drs_.Update(profiler_.total_ms());
  } else if (drs_.scale() != 1.0f) {
    drs_.Reset();
  }

  bool upscaler_changed = settings_.upscaler != applied_upscaler_ ||
                          settings_.upscaler_quality != applied_quality_ ||
                          settings_.render_scale != applied_render_scale_ ||
                          drs_.scale() != applied_dynamic_scale_;
  if (upscaler_changed) {
    device_->WaitIdle();
    upscaler_.Reset();
    if (settings_.upscaler != UpscalerKind::kNone) {
      if (!CreateUpscalerWithFallback()) {
        RX_WARN("upscaler unavailable, falling back to taa");
        settings_.upscaler = UpscalerKind::kNone;
        settings_.aa_mode = AntiAliasingMode::kTaa;
      }
    } else if (settings_.aa_mode == AntiAliasingMode::kUpscaler) {
      settings_.aa_mode = AntiAliasingMode::kTaa;
    }
    applied_upscaler_ = settings_.upscaler;
    applied_quality_ = settings_.upscaler_quality;
    applied_render_scale_ = settings_.render_scale;
    if (drs_.scale() != applied_dynamic_scale_) {
      applied_dynamic_scale_ = drs_.scale();
      RX_INFO("drs: render scale {:.0f}% (gpu {:.2f} ms, target {:.2f} ms)",
              applied_dynamic_scale_ * 100.0f, profiler_.total_ms(),
              settings_.dynamic_target_ms);
    }
    UpdateRenderResolution();
    transient_pool_->Clear();
    ResizeSizedPasses();
    taa_.Reset();
    has_prev_frame_ = false;
  }

  if (settings_.aa_mode != applied_aa_) {
    bool resolution_changes =
        settings_.aa_mode == AntiAliasingMode::kUpscaler ||
        applied_aa_ == AntiAliasingMode::kUpscaler;
    applied_aa_ = settings_.aa_mode;
    if (resolution_changes) {
      device_->WaitIdle();
      UpdateRenderResolution();
      transient_pool_->Clear();
      ResizeSizedPasses();
    }
    taa_.Reset();
    has_prev_frame_ = false;
  }

  taa_.Configure({.history_blend = settings_.taa_history_blend,
                  .jitter_sample_count = taa_.settings().jitter_sample_count});
  rtao_.Configure(
      {.radius = settings_.ao_radius,
       .ray_count = settings_.ao_rays == 0 ? 1 : settings_.ao_rays});
  ssao_.Configure(
      {.radius = settings_.ao_radius,
       .intensity = settings_.ao_intensity * 1.8f,
       .power = 1.5f,
       .sample_count = rx::Clamp(settings_.ao_rays * 8u, 4u, 32u)});
  shadow_.Configure({.cascade_count = ShadowPass::kMaxCascades,
                     .resolution = settings_.shadow_resolution,
                     .distance = settings_.shadow_distance});
  exposure_.Configure({.automatic = settings_.auto_exposure,
                       .compensation = settings_.exposure,
                       .adaptation_speed = settings_.adaptation_speed,
                       .manual_exposure = settings_.exposure});
  if (ddgi_) {
    ddgi_->Configure({.probe_spacing = settings_.ddgi_spacing,
                      .hysteresis = 0.97f,
                      .energy_scale = settings_.ddgi_intensity});
  }
  // Lazily create RCGI (~85 MiB) the first time it is switched on, so a device
  // that never enables it pays nothing and creation failure is non-fatal (the
  // feature just stays unavailable). Created once, kept across toggle-off so a
  // rapid on/off does not thrash the allocation. Needs bindless for cache
  // shading. Available on RT hardware (hw + sw pipelines so RX_RCGI_SW A/B
  // works) OR on a non-ray-query device once the SDF clipmap is up
  // (software-only pipelines); `rt_available_` selects which pipelines are
  // built.
  bool rcgi_sw_possible = sdf_available_ && sdf_clipmap_ != nullptr;
  // Honest failure for a late/programmatic rcgi enable on a non-RT device that
  // never seeded the SDF path at startup: the software tracer cannot come up
  // (SDF availability is a startup decision, see Initialize),
  // so say so once rather than silently leaving rcgi doing nothing. (The
  // debug-UI rcgi toggle is already greyed out when the device lacks ray
  // query.)
  if (settings_.rcgi && !rt_available_ && !rcgi_sw_possible &&
      !rcgi_sw_unavailable_logged_) {
    RX_WARN("rcgi: requested but the software SDF path was not enabled at "
            "startup; set RX_RCGI "
            "(or RX_SDF) before launch on a non-ray-query device. Ignoring.");
    rcgi_sw_unavailable_logged_ = true;
  }
  if (settings_.rcgi && (rt_available_ || rcgi_sw_possible) && bindless_ &&
      environment_ && !rcgi_ && !rcgi_create_failed_) {
    rcgi_ =
        RcgiSystem::Create(*device_, environment_->sky_view(),
                           environment_->sampler(), *bindless_, rt_available_);
    if (rcgi_ && light_grid_.Initialize(*device_)) {
      RX_INFO("rcgi: created on first activation (~85 MiB)");
    } else {
      RX_ERROR("rcgi: creation failed; feature unavailable this session");
      if (rcgi_)
        light_grid_.Destroy(*device_);
      rcgi_.Reset();
      rcgi_create_failed_ = true; // do not retry every frame
    }
  }
  if (rcgi_)
    rcgi_->Configure({.hysteresis = 0.97f, .energy_scale = 1.0f});

  Vec3 sun = Normalize(settings_.sun_direction);
  bool sun_changed = sun.x != applied_sun_direction_.x ||
                     sun.y != applied_sun_direction_.y ||
                     sun.z != applied_sun_direction_.z ||
                     settings_.sun_intensity != applied_sun_intensity_ ||
                     settings_.sun_color.x != applied_sun_color_.x ||
                     settings_.sun_color.y != applied_sun_color_.y ||
                     settings_.sun_color.z != applied_sun_color_.z;
  if (sun_changed) {
    applied_sun_direction_ = sun;
    applied_sun_intensity_ = settings_.sun_intensity;
    applied_sun_color_ = settings_.sun_color;
    // Re-bake once the change would show: 0.1 degrees of sun travel (under half
    // the disk's radius) or 1% of intensity or color. A clock-driven sun then
    // re-bakes every second or so instead of every frame.
    constexpr f32 kCosBakeAngle = 0.99999848f;  // cos(0.1 deg)
    auto differs = [](f32 a, f32 b) {
      return ::fabsf(a - b) > 0.01f * rx::Max(rx::Max(::fabsf(a), ::fabsf(b)), 1e-3f);
    };
    if (Dot(sun, env_baked_sun_direction_) < kCosBakeAngle ||
        differs(settings_.sun_intensity, env_baked_sun_intensity_) ||
        differs(settings_.sun_color.x, env_baked_sun_color_.x) ||
        differs(settings_.sun_color.y, env_baked_sun_color_.y) ||
        differs(settings_.sun_color.z, env_baked_sun_color_.z)) {
      environment_dirty_ = true;
    }
  }
}

bool Renderer::CreateFrameResources() {
  for (FrameResources &frame : frames_) {
    frame.globals =
        device_->CreateBuffer(sizeof(FrameGlobals), gpu::kBufferUsageUniform, true);
    if (!frame.globals.mapped)
      return false;

    // Bone palette: host visible, read in the skinned vertex shader through its
    // device address (no descriptor binding). Column-major 4x4 per bone.
    // Two halves of kMaxFrameBones: this frame's poses, then last frame's for
    // the motion vectors. Sized for both so a scene that fills the budget loses
    // no history - the cliff would show up as smearing on the busiest frame.
    frame.bone_palette = device_->CreateBuffer(
        static_cast<u64>(kMaxFrameBones) * 2 * sizeof(Mat4),
        gpu::kBufferUsageStorage | gpu::kBufferUsageDeviceAddress, true);
    if (!frame.bone_palette.mapped)
      return false;

    // Morph weights: host visible (target, weight) pairs, read like the bones.
    frame.morph_weights = device_->CreateBuffer(
        static_cast<u64>(kMaxFrameMorphWeights) * sizeof(MorphWeight),
        gpu::kBufferUsageStorage | gpu::kBufferUsageDeviceAddress, true);
    if (!frame.morph_weights.mapped)
      return false;

    frame.lights = device_->CreateBuffer(static_cast<u64>(kMaxFrameLights) *
                                             sizeof(PointLight),
                                         gpu::kBufferUsageStorage, true);
    if (!frame.lights.mapped)
      return false;
    frame.decals =
        device_->CreateBuffer(static_cast<u64>(kMaxFrameDecals) * sizeof(Decal),
                              gpu::kBufferUsageStorage, true);
    if (!frame.decals.mapped)
      return false;

    // Per-draw transform arena. Sized for a typical scene here and grown by
    // UploadDrawRecords when a frame needs more.
    frame.draw_record_capacity = 1024;
    frame.draw_records = device_->CreateBuffer(
        static_cast<u64>(frame.draw_record_capacity) * sizeof(DrawRecord),
        gpu::kBufferUsageStorage, true);
    if (!frame.draw_records.mapped)
      return false;
  }
  return true;
}

const gpu::GpuBuffer &Renderer::UploadDrawRecords(FrameResources &frame,
                                             const FrameView &view) {
  // Record 0 is the kNoDrawRecord slot an instanced draw points at, so the arena
  // is one longer than the draw list and every draw's record is 1 + its index.
  const u32 needed = static_cast<u32>(view.draws.size()) + 1;
  if (frame.draw_record_capacity < needed) {
    // The slot's fence fired in BeginFrame, so nothing still reads the old
    // buffer; retire it deferred anyway and round up to keep growth rare.
    u32 cap = frame.draw_record_capacity ? frame.draw_record_capacity : 1024;
    while (cap < needed)
      cap *= 2;
    device_->DestroyBufferDeferred(frame.draw_records);
    frame.draw_records =
        device_->CreateBuffer(static_cast<u64>(cap) * sizeof(DrawRecord),
                              gpu::kBufferUsageStorage, true);
    frame.draw_record_capacity = frame.draw_records.mapped ? cap : 0;
  }
  if (!frame.draw_records.mapped)
    return frame.draw_records;
  auto *records = static_cast<DrawRecord *>(frame.draw_records.mapped);
  records[0] = {Mat4::Identity(), Mat4::Identity()};
  for (size_t i = 0; i < view.draws.size(); ++i) {
    records[i + 1] = {view.draws[i].transform, view.draws[i].prev_transform};
  }
  return frame.draw_records;
}

void Renderer::DestroyFrameResources() {
  for (FrameResources &frame : frames_) {
    if (frame.globals)
      device_->DestroyBuffer(frame.globals);
    if (frame.bone_palette)
      device_->DestroyBuffer(frame.bone_palette);
    if (frame.morph_weights)
      device_->DestroyBuffer(frame.morph_weights);
    if (frame.lights)
      device_->DestroyBuffer(frame.lights);
    if (frame.decals)
      device_->DestroyBuffer(frame.decals);
    if (frame.draw_records)
      device_->DestroyBuffer(frame.draw_records);
    frame = {};
  }
  for (u32 i = 0; i < kFramesInFlight; ++i) {
    device_->DestroyBindingSet(globals_sets_[i]);
    device_->DestroyBindingSet(env_scene_sets_[i]);
    device_->DestroyBindingSet(env_prepass_sets_[i]);
    device_->DestroyBindingSet(env_transparent_sets_[i]);
    globals_sets_[i] = {};
    env_scene_sets_[i] = {};
    env_transparent_sets_[i] = {};
  }
}

bool Renderer::WantHdrSwapchain() const {
  return settings_.hdr_output && window_ && window_->hdr_enabled();
}

void Renderer::RecreateSwapchain() {
  if (!window_)
    return; // windowless: the offscreen stand-in never resizes
  u32 width = window_->width();
  u32 height = window_->height();
  if (width == 0 || height == 0)
    return; // minimized
  device_->WaitIdle();
  swapchain_.Reset();
  swapchain_starved_ = false; // a fresh swapchain is worth probing again
  swapchain_hdr_request_ = WantHdrSwapchain();
  swapchain_ = device_->CreateSwapchain(width, height, settings_.vsync,
                                        swapchain_hdr_request_);
  if (!swapchain_)
    return;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;

  // The upscaler is sized for the output, rebuild it alongside. Same fallback
  // as first bringup: a resize is another chance for a vendor runtime to fail,
  // and dropping a working upscale to taa on a window resize is not something
  // anyone would connect back to the resize.
  if (upscaler_) {
    upscaler_.Reset();
    if (!CreateUpscalerWithFallback()) {
      settings_.upscaler = UpscalerKind::kNone;
      settings_.aa_mode = AntiAliasingMode::kTaa;
      applied_upscaler_ = UpscalerKind::kNone;
    }
  }
  // The frame generator is sized for the swapchain; lazily recreated.
  framegen_.Reset();
  framegen_attempted_ = false;
  framegen_was_active_ = false;
  UpdateRenderResolution();
  transient_pool_->Clear();
  // Resize every render-resolution pass through the shared helper rather than a
  // partial hand-rolled copy: this list had drifted and omitted the NRD
  // denoiser (and vrs/restir/rr), so the SIGMA sun-shadow history stayed at the
  // old resolution - shadows kept the pre-resize size and ghosted at the wrong
  // framebuffer position until the history flushed.
  ResizeSizedPasses();
  taa_.Reset();
  has_prev_frame_ = false;
}

void Renderer::DestroySurface() {
  if (!device_ || device_->is_stub())
    return;
  device_->WaitIdle();
  swapchain_.Reset();
  device_->DestroySurface();
}

void Renderer::RecreateSurface() {
  if (!device_ || device_->is_stub() || !window_)
    return;
  if (!device_->RecreateSurface(*window_))
    return;
  RecreateSwapchain(); // rebuilds the swapchain and sized targets
}

void Renderer::WaitIdle() {
  if (device_ && !device_->is_stub())
    device_->WaitIdle();
}

void Renderer::Shutdown() {
  if (device_ && !device_->is_stub()) {
    device_->WaitIdle();
    DestroyFrameResources();
    instances_.Shutdown(*device_);
    for (auto kv : meshes_) {
      device_->DestroyBuffer(kv.value.vertices);
      device_->DestroyBuffer(kv.value.indices);
      if (kv.value.skinning)
        device_->DestroyBuffer(kv.value.skinning);
      if (kv.value.morph_deltas)
        device_->DestroyBuffer(kv.value.morph_deltas);
      if (kv.value.meshlets)
        device_->DestroyBuffer(kv.value.meshlets);
      if (kv.value.meshlet_vertices)
        device_->DestroyBuffer(kv.value.meshlet_vertices);
      if (kv.value.meshlet_triangles)
        device_->DestroyBuffer(kv.value.meshlet_triangles);
      if (kv.value.rt_approx_vertices)
        device_->DestroyBuffer(kv.value.rt_approx_vertices);
      if (kv.value.rt_approx_indices)
        device_->DestroyBuffer(kv.value.rt_approx_indices);
      for (gpu::GpuMesh::LodRt &rt : kv.value.lod_rt)
        if (rt.indices)
          device_->DestroyBuffer(rt.indices);
    }
    meshes_.clear();
    taa_.Destroy(*device_);
    ssao_.Destroy(*device_);
    ssr_.Destroy(*device_);
    ssgi_.Destroy(*device_);
    if (rcgi_)
      light_grid_.Destroy(*device_); // rcgi_ (unique_ptr) frees itself
    // Free SDF GPU resources while the device is still valid (the unique_ptr
    // destructors call DestroyImage/DestroyBuffer/DestroyPipeline).
    sdf_clipmap_.Reset();
    sdf_scene_.Reset();
    device_->DestroyPipeline(hdr_pipeline_);
    hdr_pipeline_ = {};
    device_->DestroyBuffer(hdr_readback_);
    shadow_.Destroy(*device_);
    local_shadows_.Destroy(*device_);
    froxel_fog_.Destroy(*device_);
    particles_.Destroy(*device_);
    procedural_grass_.Destroy(*device_);
    gaussians_.Destroy(*device_);
    fur_.Destroy(*device_);
    wboit_.Destroy(*device_);
    overdraw_.Destroy(*device_);
    gpu_cull_.Destroy(*device_);
    skinned_rt_.Destroy(*device_);
    meshlet_.Destroy(*device_);
    if (ms_dummy_hiz_)
      device_->DestroyImage(ms_dummy_hiz_);
    if (rt_available_)
      rtao_.Destroy(*device_);
    if (rt_available_)
      reflection_trace_.Destroy(*device_);
    motion_blur_.Destroy(*device_);
    dof_.Destroy(*device_);
    if (light_cluster_pipeline_)
      device_->DestroyPipeline(light_cluster_pipeline_);
    if (msaa_resolve_pipeline_)
      device_->DestroyPipeline(msaa_resolve_pipeline_);
    if (depth_copy_pipeline_)
      device_->DestroyPipeline(depth_copy_pipeline_);
    if (hdr_overlay_copy_pipeline_)
      device_->DestroyPipeline(hdr_overlay_copy_pipeline_);
    if (contact_shadow_pipeline_)
      device_->DestroyPipeline(contact_shadow_pipeline_);
    if (cloud_shadow_pipeline_)
      device_->DestroyPipeline(cloud_shadow_pipeline_);
    if (sss_pipeline_)
      device_->DestroyPipeline(sss_pipeline_);
    // Editor debug-line + picking resources (lazily created).
    if (debug_line_pipeline_)
      device_->DestroyPipeline(debug_line_pipeline_);
    if (debug_line_overlay_pipeline_)
      device_->DestroyPipeline(debug_line_overlay_pipeline_);
    for (gpu::GpuBuffer &vbo : debug_line_vbo_)
      if (vbo)
        device_->DestroyBuffer(vbo);
    if (pick_pipeline_)
      device_->DestroyPipeline(pick_pipeline_);
    if (pick_id_image_)
      device_->DestroyImage(pick_id_image_);
    if (pick_depth_image_)
      device_->DestroyImage(pick_depth_image_);
    if (cluster_counts_)
      device_->DestroyBuffer(cluster_counts_);
    if (cluster_indices_)
      device_->DestroyBuffer(cluster_indices_);
    if (decal_cluster_indices_)
      device_->DestroyBuffer(decal_cluster_indices_);
    for (gpu::GpuBuffer &camera : contact_camera_) {
      if (camera)
        device_->DestroyBuffer(camera);
      camera = {};
    }
#if defined(RX_HAS_NRD)
    if (rt_available_)
      nrd_.Destroy(*device_);
    if (rt_available_)
      shadow_trace_.Destroy(*device_);
#endif
#if defined(RX_HAS_DLSS)
    rr_.Destroy(*device_);
#endif
    bloom_.Destroy(*device_);
    exposure_.Destroy(*device_);
    reference_compare_.Destroy(*device_);
    vrs_.Destroy(*device_);
    restir_di_.Destroy(*device_);
    virtual_texture_.Destroy(*device_);
    decal_baker_.Destroy(*device_);
    vgeo_.Destroy(*device_);
    hair_.Destroy(*device_);
    ocean_.Destroy(*device_);
    water_field_.Destroy(*device_);
    fluid_sim_.Destroy(*device_);
    shore_wetting_.Destroy(*device_);
    water_caustics_.Destroy(*device_);
    imposters_.Destroy(*device_);
    profiler_.Shutdown();
    path_tracer_.Destroy(*device_);
    recon_path_tracer_.Destroy(*device_);
    volumetric_fog_.Destroy(*device_);
    aerial_perspective_.Destroy(*device_);
    clouds_.Destroy(*device_);
    if (cloudscape_ready_)
      cloudscape_.Destroy(*device_);
    cloudscape_ready_ = false;
    cloudscape_init_tried_ = false;
    applied_cloudscape_ = false;
    precipitation_.Destroy(*device_);
    precip_occlusion_.Destroy(*device_);
    precip_volume_.Destroy(*device_);
    lightning_.Destroy(*device_);
    surface_weather_.Destroy(*device_);
    water_.Reset();
    fluid_surface_.Reset();
    ddgi_.Reset();
    rcgi_.Reset(); // owns GPU resources through device_; destroy before device
                   // teardown
    environment_.Reset();
    material_system_.Reset();
    bindless_.Reset();
    transient_pool_.Reset();
  }
  graph_.Reset();
  if (capture_image_.handle)
    device_->DestroyImage(capture_image_); // before device_ goes away
  post_.Reset();
  ui_blur_.Reset(); // holds a Device& + backend handles; destroy before device_
  mesh_pipeline_.Reset();
  swapchain_.Reset();
  framegen_.Reset(); // ffx contexts destroy through the device
  upscaler_.Reset();
  raytracing_.Reset();
  device_.Reset();
}

void Renderer::LogTextureMemory() const {
  if (!material_system_)
    return;
  auto mb = [](u64 bytes) { return static_cast<f64>(bytes) / (1024.0 * 1024.0); };
  const MaterialSystem::StreamingStats stats = material_system_->streaming_stats();
  const asset::TextureCompressionStats compression = asset::CompressionTotals();
  RX_INFO("material textures: {:.2f} MB resident in {} textures, budget {} MB, {} streamable "
          "({} at their tail)",
          mb(stats.resident_bytes), material_system_->texture_count(),
          stats.budget_bytes >> 20, stats.streamable_count, stats.demoted_count);
  if (compression.compressed == 0 && compression.skipped == 0) {
    RX_INFO("texture compression: nothing compressed (off, or nothing reached the importer)");
    return;
  }
  RX_INFO("texture compression: {} textures {:.2f} -> {:.2f} MB ({:.2f}x), {} left "
          "uncompressed, {} from cache, {:.2f}s encoding",
          compression.compressed, mb(compression.source_bytes), mb(compression.compressed_bytes),
          compression.compressed_bytes
              ? static_cast<f64>(compression.source_bytes) /
                    static_cast<f64>(compression.compressed_bytes)
              : 0.0,
          compression.skipped, compression.cache_hits, compression.encode_seconds);
  // Otherwise the "left uncompressed" count above reads as a failure when it is
  // a setting: normal maps are the bulk of it on a normal-mapped scene.
  if (compression.skipped > 0 && !asset::TextureCompressionSettings().normals) {
    RX_INFO("texture compression: tangent-space normal maps are excluded; "
            "RX_TEX_COMPRESS_NORMALS=1 includes them (see asset/texture_compress.h)");
  }
}

const gpu::DeviceCaps *Renderer::caps() const {
  return device_ ? &device_->caps() : nullptr;
}

void Renderer::ClearFrameCallbacks() { graph_.Reset(); }

gpu::Format Renderer::swapchain_format() const {
  return swapchain_ ? swapchain_->format() : gpu::Format::kUnknown;
}

u32 Renderer::swapchain_image_count() const {
  return swapchain_ ? swapchain_->image_count() : 0;
}

} // namespace rx::render
