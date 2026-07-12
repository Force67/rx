#include "render/core/renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <base/option.h>

#include <stb_image_write.h>

#include "asset/primitives.h"
#include "core/log.h"
#include "core/memory/memory_tracker.h"
#include "render/util/exr_write.h"
#include "shaders/cloud_shadow_cs_hlsl.h"
#include "shaders/contact_shadow_cs_hlsl.h"
#include "shaders/debug_line_ps_hlsl.h"
#include "shaders/debug_line_vs_hlsl.h"
#include "shaders/depth_copy_ps_hlsl.h"
#include "shaders/fullscreen_vs_hlsl.h"
#include "shaders/hdr_capture_cs_hlsl.h"
#include "shaders/light_cluster_cs_hlsl.h"
#include "shaders/msaa_resolve_cs_hlsl.h"
#include "shaders/pick_id_ps_hlsl.h"
#include "shaders/pick_id_vs_hlsl.h"
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
base::Option<double> GrainOpt{"film.grain", 0.015, "RX_FILM_GRAIN"};
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
base::Option<int> TexBudgetMb{"tex.budget.mb", -1, "RX_TEX_BUDGET_MB"};
base::Option<bool> GpuTimings{"gpu.timings", false, "RX_GPU_TIMINGS"};
base::Option<int> MsaaOpt{"msaa", 0, "RX_MSAA"};
base::Option<bool> DrsOpt{"drs", false, "RX_DRS"};
base::Option<double> DrsTargetMs{"drs.target.ms", 16.6, "RX_DRS_TARGET_MS"};
base::Option<double> DrsMinScale{"drs.min.scale", 0.5, "RX_DRS_MIN_SCALE"};
base::Option<bool> VrsOpt{"vrs", true, "RX_VRS"};
base::Option<double> VrsThreshold{"vrs.threshold", 0.06, "RX_VRS_THRESHOLD"};
base::Option<bool> RestirDiOpt{"restir.di", false, "RX_RESTIR_DI"};
base::Option<bool> RcgiOpt{"rcgi", false, "RX_RCGI"};
// A/B debug: use the M1 per-pixel cascade resolve instead of the M2 gather
// chain.
base::Option<bool> RcgiProbesOnlyOpt{"rcgi.probes_only", false,
                                     "RX_RCGI_PROBES_ONLY"};
// Force RCGI's world side through the software SDF tracer (no ray query).
// Implied on non-ray-query devices; on RT hardware this is the A/B toggle vs
// the TLAS path. Implies the SDF clipmap cost (RX_SDF is auto-enabled when RCGI
// needs it).
base::Option<bool> RcgiSwOpt{"rcgi.software", false, "RX_RCGI_SW"};
// Phase 3 leak/occlusion hardening. All default on; each isolates one fix for
// A/B (interior ambient miss + volume classify; probe relocation; probe AO).
base::Option<bool> RcgiInteriorOpt{"rcgi.interior", true, "RX_RCGI_INTERIOR"};
base::Option<bool> RcgiRelocateOpt{"rcgi.relocate", true, "RX_RCGI_RELOCATE"};
base::Option<bool> RcgiProbeAoOpt{"rcgi.probe_ao", true, "RX_RCGI_PROBE_AO"};
// RCGI final-gather resolution scale: 2 = half res (default), 4 = quarter res
// (opt-in; AC Shadows shipped quarter-res diffuse on consoles). The denoise
// radius widens automatically at quarter to keep the cornell/interior clean.
base::Option<int> RcgiGatherScaleOpt{"rcgi.gather_scale", 2,
                                     "RX_RCGI_GATHER_SCALE"};
// Material-ID denoiser mask (item 22): reject cross-class neighbours in the
// RCGI spatial/temporal filters. On by default; env for A/B.
base::Option<bool> RcgiDenoiseMaskOpt{"rcgi.denoise_mask", true,
                                      "RX_RCGI_DENOISE_MASK"};
// SDF software-trace infrastructure (S1): mesh SDFs + global SDF clipmap. Off
// by default; RX_SDF_DEBUG raymarches the clipmap (1 = distance field, 2 =
// albedo).
base::Option<bool> SdfOpt{"sdf", false, "RX_SDF"};
base::Option<int> SdfDebugOpt{"sdf.debug", 0, "RX_SDF_DEBUG"};
base::Option<float> VgeoError{"vgeo.error", 1.0f, "RX_VGEO_ERROR"};
// 0 shaded, 1 cluster tint, 2 lod tint, 3 sw/hw raster path.
base::Option<int> VgeoDebug{"vgeo.debug", 1, "RX_VGEO_DEBUG"};
// Alpha-tested vegetation in the rays (AC Shadows opaque-approximation).
// RX_RT_VEG shrinks each masked mesh's realtime stand-in by its baked average
// opacity so realtime diffuse GI / AO / shadow rays get correct-on-average
// foliage occlusion; 0 forces the stand-in to full size (today's force-opaque
// behavior). RX_RT_VEG_ANYHIT switches specular reflections to a bounded
// real-alpha any-hit test (the approximation reads wrong in sharp reflections).
base::Option<bool> RtVegOpt{"rt.veg", true, "RX_RT_VEG"};
base::Option<bool> RtVegAnyHitOpt{"rt.veg.anyhit", true, "RX_RT_VEG_ANYHIT"};
// Phase 4 specular reflection quality/perf levers (AC Shadows adoption).
base::Option<bool> ReflHalfOpt{"refl.half", true, "RX_REFL_HALF"};
base::Option<bool> ReflShSkipOpt{"refl.sh_skip", true, "RX_REFL_SH_SKIP"};
base::Option<float> ReflShSkipRough{"refl.sh_skip.rough", 0.45f,
                                    "RX_REFL_SH_SKIP_ROUGH"};
base::Option<bool> ReflFogOpt{"refl.fog", true, "RX_REFL_FOG"};
// RT scene scalability (AC Shadows §2). RX_RT_CULL drops small distant
// instances from the realtime TLAS by projected solid angle beyond
// RX_RT_CULL_START metres, threshold RX_RT_CULL_ANGLE (angular radius). RX_RT_
// LOD_NEAR keeps LOD0 in the rays within that radius (raster/RT agree in the
// screen-trace range) and lets distance LODs into the BLAS past it. RX_RT_
// ASYNC_TLAS builds the per-frame TLAS on the async compute queue. All are
// realtime-only: the path tracer keeps every instance at LOD0.
base::Option<bool> RtCullOpt{"rt.cull", true, "RX_RT_CULL"};
base::Option<float> RtCullAngle{"rt.cull.angle", 0.004f, "RX_RT_CULL_ANGLE"};
base::Option<float> RtCullStart{"rt.cull.start", 40.0f, "RX_RT_CULL_START"};
base::Option<float> RtLodNear{"rt.lod.near", 64.0f, "RX_RT_LOD_NEAR"};
base::Option<bool> RtAsyncTlasOpt{"rt.async.tlas", true, "RX_RT_ASYNC_TLAS"};
base::Option<bool> FftOceanOpt{"fft.ocean", true, "RX_FFT_OCEAN"};
base::Option<bool> AdaptiveWaterOpt{"water.adaptive", true,
                                    "RX_ADAPTIVE_WATER"};
base::Option<bool> WaterFieldOpt{"water.field", true, "RX_WATER_FIELD"};
base::Option<bool> FluidSimOpt{"fluid.sim", false, "RX_FLUID_SIM"};
base::Option<bool> WaterInteractionOpt{"water.interaction", true,
                                       "RX_WATER_INTERACTION"};
// Debug/verification only: force the ripple obstacle boundary off while leaving
// the shoreline-wetting shading on, so an A/B isolates ripples reflecting off
// the island from the wet-sand shading. Not wired to the ini/UI on purpose.
base::Option<bool> WaterObstacleOpt{"water.obstacle", true,
                                    "RX_WATER_OBSTACLE"};
base::Option<bool> ShoreWettingOpt{"shore.wetting", false, "RX_SHORE_WETTING"};
base::Option<bool> WaterCausticsOpt{"water.caustics", true,
                                    "RX_WATER_CAUSTICS"};
base::Option<bool> ProceduralGrassOpt{"procedural.grass", true,
                                      "RX_PROCEDURAL_GRASS"};
// Debug: horizontal fake velocity in pixels, to exercise the blur from a
// static camera (screenshot testing).
base::Option<double> MotionBlurDebugVel{"motion.blur.debug.vel", 0.0,
                                        "RX_MOTION_BLUR_DEBUG_VEL"};
base::Option<double> HdrPaperWhite{"hdr.paper.white", 200.0,
                                   "RX_HDR_PAPER_WHITE"};
// Debug: force the tonemap's output transfer (1 pq, 2 scrgb) on an SDR
// swapchain, so the encode math is testable on displays with no HDR path.
base::Option<int> HdrForceTransfer{"hdr.force.transfer", 0,
                                   "RX_HDR_FORCE_TRANSFER"};
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

// Distance-based hierarchical lod: coarser geometry the further a mesh is from
// the camera. Switches roughly every few bounding radii; clamps to the
// coarsest.
u32 SelectLod(const GpuMesh &mesh, f32 distance) {
  u32 lod_count = 1u + static_cast<u32>(mesh.lods.size());
  if (lod_count <= 1)
    return 0;
  f32 unit = std::max(mesh.bounds_radius, 0.25f) * 2.5f;
  u32 lod = static_cast<u32>(distance / std::max(unit, 0.5f));
  return lod < lod_count ? lod : lod_count - 1;
}

bool SupportsStaticInstances(const GpuMesh &mesh,
                             const MaterialSystem *materials) {
  if (mesh.all_blend || mesh.skinned || mesh.morph_target_count > 0 ||
      mesh.terrain_lod || mesh.dynamic_vertices)
    return false;
  auto supported = [materials](const base::Vector<GpuSubmesh> &submeshes) {
    for (const GpuSubmesh &submesh : submeshes) {
      if (submesh.blend ||
          (materials && materials->is_normal_model_space(submesh.material))) {
        return false;
      }
    }
    return true;
  };
  if (!supported(mesh.submeshes))
    return false;
  for (const GpuLod &lod : mesh.lods) {
    if (!supported(lod.submeshes))
      return false;
  }
  return true;
}

bool HasUniformScale(std::span<const Mat4> transforms) {
  for (const Mat4 &transform : transforms) {
    const f32 *m = transform.m;
    for (u32 i = 0; i < 16; ++i)
      if (!std::isfinite(m[i]))
        return false;
    if (std::abs(m[3]) > 1e-5f || std::abs(m[7]) > 1e-5f ||
        std::abs(m[11]) > 1e-5f || std::abs(m[15] - 1.0f) > 1e-5f)
      return false;
    const f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const f32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    const f32 tolerance = std::max({sx, sy, sz}) * 1e-4f;
    if (sx <= 1e-6f || std::abs(sx - sy) > tolerance ||
        std::abs(sx - sz) > tolerance)
      return false;
    const f32 orthogonal_tolerance = sx * sx * 1e-4f;
    const f32 determinant = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                            m[4] * (m[1] * m[10] - m[2] * m[9]) +
                            m[8] * (m[1] * m[6] - m[2] * m[5]);
    if (determinant <= 0 ||
        std::abs(m[0] * m[4] + m[1] * m[5] + m[2] * m[6]) >
            orthogonal_tolerance ||
        std::abs(m[0] * m[8] + m[1] * m[9] + m[2] * m[10]) >
            orthogonal_tolerance ||
        std::abs(m[4] * m[8] + m[5] * m[9] + m[6] * m[10]) >
            orthogonal_tolerance)
      return false;
  }
  return true;
}

f32 InstanceGroupDistance(const InstanceStore::Group &group, const Vec3 &eye) {
  const Vec3 delta = eye - group.bounds_center;
  return std::max(std::sqrt(Dot(delta, delta)) - group.bounds_radius, 0.0f) /
         group.lod_scale;
}

// Gribb-Hartmann frustum planes (left,right,bottom,top,near) from a
// column-major view_proj, normalized so a point is inside when dot(n,p)+d >= 0.
// Far is skipped.
void ExtractFrustumPlanes(const Mat4 &vp, f32 out[5][4]) {
  const f32 *m = vp.m;
  auto row = [&](int r, int c) { return m[c * 4 + r]; };
  f32 p[5][4] = {
      {row(3, 0) + row(0, 0), row(3, 1) + row(0, 1), row(3, 2) + row(0, 2),
       row(3, 3) + row(0, 3)},
      {row(3, 0) - row(0, 0), row(3, 1) - row(0, 1), row(3, 2) - row(0, 2),
       row(3, 3) - row(0, 3)},
      {row(3, 0) + row(1, 0), row(3, 1) + row(1, 1), row(3, 2) + row(1, 2),
       row(3, 3) + row(1, 3)},
      {row(3, 0) - row(1, 0), row(3, 1) - row(1, 1), row(3, 2) - row(1, 2),
       row(3, 3) - row(1, 3)},
      {row(2, 0), row(2, 1), row(2, 2), row(2, 3)},
  };
  for (int i = 0; i < 5; ++i) {
    f32 len =
        std::sqrt(p[i][0] * p[i][0] + p[i][1] * p[i][1] + p[i][2] * p[i][2]);
    if (len < 1e-8f)
      len = 1.0f;
    for (int c = 0; c < 4; ++c)
      out[i][c] = p[i][c] / len;
  }
}

// World-space sphere vs the normalized frustum planes; outside if it falls
// beyond any plane. Lets the cpu skip a draw entirely (no dispatch) for
// off-screen instances, so cpu cost tracks visible instances rather than
// streamed ones.
bool SphereOutsideFrustum(const f32 planes[5][4], const Vec3 &c, f32 r) {
  for (int i = 0; i < 5; ++i) {
    if (planes[i][0] * c.x + planes[i][1] * c.y + planes[i][2] * c.z +
            planes[i][3] <
        -r) {
      return true;
    }
  }
  return false;
}

// Average opacity of an alpha-masked submesh for the vegetation opaque
// approximation. Samples the material's baked alpha grid at each triangle's
// three vertices, edge midpoints and centroid (7 points in barycentric UV
// space -- a cheap stand-in for integrating covered texels over the footprint)
// and area-weights across the submesh. Returns 1.0 (no shrink, i.e. today's
// force-opaque behavior) when the alpha was not decoded: opaque texture, a
// format without a CPU alpha decoder (BC7), or a missing material.
f32 MaskedSubmeshOpacity(const base::Vector<asset::Vertex> &verts,
                         const base::Vector<u32> &indices, const GpuSubmesh &sm,
                         const MaterialSystem::AlphaCoverage *cov) {
  if (!cov)
    return 1.0f;
  auto S = [&](const f32 uv0[2], const f32 uv1[2], const f32 uv2[2], f32 w0,
               f32 w1, f32 w2) {
    return cov->Sample(uv0[0] * w0 + uv1[0] * w1 + uv2[0] * w2,
                       uv0[1] * w0 + uv1[1] * w1 + uv2[1] * w2);
  };
  f64 weighted = 0.0, area_sum = 0.0;
  for (u32 e = 0; e + 3 <= sm.index_count; e += 3) {
    u32 i0 = indices[sm.index_offset + e],
        i1 = indices[sm.index_offset + e + 1],
        i2 = indices[sm.index_offset + e + 2];
    if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size())
      continue;
    const asset::Vertex &a = verts[i0];
    const asset::Vertex &b = verts[i1];
    const asset::Vertex &c = verts[i2];
    f32 mean_a =
        (S(a.uv, b.uv, c.uv, 1, 0, 0) + S(a.uv, b.uv, c.uv, 0, 1, 0) +
         S(a.uv, b.uv, c.uv, 0, 0, 1) + S(a.uv, b.uv, c.uv, 0.5f, 0.5f, 0) +
         S(a.uv, b.uv, c.uv, 0, 0.5f, 0.5f) +
         S(a.uv, b.uv, c.uv, 0.5f, 0, 0.5f) +
         S(a.uv, b.uv, c.uv, 1.f / 3, 1.f / 3, 1.f / 3)) /
        7.0f;
    f32 e1[3] = {b.position[0] - a.position[0], b.position[1] - a.position[1],
                 b.position[2] - a.position[2]};
    f32 e2[3] = {c.position[0] - a.position[0], c.position[1] - a.position[1],
                 c.position[2] - a.position[2]};
    f32 cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2],
        cz = e1[0] * e2[1] - e1[1] * e2[0];
    f32 area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
    weighted += static_cast<f64>(mean_a) * area;
    area_sum += area;
  }
  if (area_sum <= 0.0)
    return cov->mean;
  return static_cast<f32>(weighted / area_sum);
}

} // namespace

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::Initialize(const RendererDesc &desc, Window &window) {
#if defined(RX_SHARED_BUILD)
  // base::Option self-registers through base::InitChain, whose list head is a
  // vague-linkage function-local static. Under RX_SHARED every module is built
  // with hidden visibility, so that head is a SEPARATE instance per DSO: the
  // app's InitOptionsFromEnv() (host.cc) only walks the app DSO's chain and
  // never sees this DSO's render options (RX_ASYNC_COMPUTE, RX_DEBUG_VIEW,
  // ...). Re-run it here so the render DSO applies its own env overrides before
  // any option is read. Disjoint chains, so no option is applied twice. In the
  // static build there is one shared chain and this is compiled out.
  base::InitOptionsFromEnv();
#endif
  desc_ = desc;
  settings_.aa_mode = desc.aa_mode;
  settings_.upscaler = desc.upscaler;
  settings_.rt_shadows = desc.raytracing.shadows;
  output_width_ = window.width();
  output_height_ = window.height();
  // Applied before the swapchain exists (the rest of the option overrides run
  // later in Initialize; these two decide the surface format).
  if (HdrOutput.overridden())
    settings_.hdr_output = HdrOutput;
  if (HdrPaperWhite.overridden())
    settings_.hdr_paper_white = static_cast<f32>(double(HdrPaperWhite));

  // RX_RHI=vulkan|d3d12|null|auto overrides the graphics backend.
  Backend backend = desc.backend;
  if (const char *name = RhiBackend.get()) {
    std::string value = name;
    if (value == "vulkan")
      backend = Backend::kVulkan;
    else if (value == "d3d12")
      backend = Backend::kD3D12;
    else if (value == "null")
      backend = Backend::kNull;
    else if (value == "auto")
      backend = Backend::kAuto;
    else
      RX_WARN("RX_RHI: unknown backend '{}', using {}", value,
              BackendName(backend));
  }

  window_ = &window;
  device_ = Device::Create({.backend = backend,
                            .enable_validation = desc.enable_validation,
                            .request_raytracing = desc.enable_raytracing,
                            .extra_device_extensions = desc.vulkan.extensions},
                           window);
  if (device_->is_stub()) {
    RX_WARN("renderer running in stub mode");
    return true;
  }

  // Everything below creates pipelines; batch them so the driver compiles
  // across cores on a cold cache (the guard joins the workers on every exit
  // path, and a failed compile fails Initialize at the End check).
  struct PipelineBatchGuard {
    Device &device;
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
  auto t_batch0 = std::chrono::steady_clock::now();
  const char *pso_batch_env = std::getenv("RX_PSO_BATCH");
  if (!pso_batch_env || pso_batch_env[0] != '0')
    device_->BeginPipelineBatch();

  swapchain_hdr_request_ = WantHdrSwapchain();
  if (settings_.hdr_output && !swapchain_hdr_request_) {
    RX_INFO("hdr output requested but the system is not compositing in hdr; "
            "using sdr "
            "(enable hdr in the os display settings)");
  }
  swapchain_ = device_->CreateSwapchain(
      output_width_, output_height_, settings_.vsync, swapchain_hdr_request_);
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

  transient_pool_ = std::make_unique<TransientPool>(*device_);
  // The bindless registry has no ray-tracing dependency (buffers + an
  // update-after-bind set): the forward terrain splat and textured particles
  // sample through it too, so it exists on every real device. Mesh/geometry
  // registration (device-address reads) stays gated on ray tracing below.
  bindless_ = BindlessRegistry::Create(*device_);
  if (!bindless_)
    return false;
  material_system_ = MaterialSystem::Create(*device_, bindless_.get());
  if (!material_system_)
    return false;
  environment_ = EnvironmentSystem::Create(*device_);
  if (!environment_)
    return false;
  mesh_pipeline_ = MeshPipeline::Create(
      *device_, kSceneColorFormat, kMotionFormat, kNormalFormat, kDepthFormat,
      material_system_->set_layout(), environment_->env_set_layout(),
      bindless_ ? bindless_->set_layout() : BindingLayoutHandle{});
  post_ = PostPass::Create(*device_, swapchain_->format());
  if (!mesh_pipeline_ || !post_ || !taa_.Initialize(*device_))
    return false;
  // kMsaa support: sample-0 guide resolve + the fullscreen depth rebuild that
  // hands the post-resolve raster passes a single-sampled depth buffer.
  msaa_resolve_pipeline_ = device_->CreateComputePipeline({
      .shader = RX_SHADER(k_msaa_resolve_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kSampledImage},
                          {1, BindingType::kSampledImage},
                          {2, BindingType::kStorageImage},
                          {3, BindingType::kStorageImage}}}},
      .push_constant_size = 8,
      .debug_name = "msaa_resolve",
  });
  depth_copy_pipeline_ = device_->CreateGraphicsPipeline({
      .vertex = RX_SHADER(k_fullscreen_vs_hlsl),
      .fragment = RX_SHADER(k_depth_copy_ps_hlsl),
      .raster = {.cull = CullMode::kNone},
      .depth = {.test = true,
                .write = true,
                .compare = CompareOp::kAlways,
                .format = kDepthFormat},
      .sets = {{.slots = {{0, BindingType::kSampledImage}}}},
      .debug_name = "msaa_depth_copy",
  });
  if (!msaa_resolve_pipeline_ || !depth_copy_pipeline_)
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
        .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                            {1, BindingType::kStorageBuffer},
                            {2, BindingType::kStorageBuffer},
                            {3, BindingType::kStorageBuffer},
                            {4, BindingType::kStorageBuffer}}}},
        .push_constant_size = sizeof(ClusterPush),
        .debug_name = "light_cluster",
    });
    if (!light_cluster_pipeline_)
      return false;
    struct ContactPush {
      Mat4 view_proj;
      Mat4 inv_view_proj;
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
        .sets = {{.slots = {{0, BindingType::kStorageImage},
                            {1, BindingType::kSampledImage}}}},
        .push_constant_size = sizeof(ContactPush),
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
        .sets = {{.slots = {{0, BindingType::kStorageImage},
                            {1, BindingType::kSampledImage}}}},
        .push_constant_size = sizeof(CloudShadowPush),
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
        .sets = {{.slots = {{0, BindingType::kStorageImage},
                            {1, BindingType::kCombinedTextureSampler},
                            {2, BindingType::kCombinedTextureSampler},
                            {3, BindingType::kCombinedTextureSampler}}}},
        .push_constant_size = sizeof(SssPush),
        .debug_name = "sss_blur",
    });
    if (!sss_pipeline_)
      return false;
    sss_sampler_ =
        device_->GetSampler({.address_u = AddressMode::kClampToEdge,
                             .address_v = AddressMode::kClampToEdge});
    cluster_counts_ =
        device_->CreateBuffer(kClusterCount * sizeof(u32), kBufferUsageStorage);
    cluster_indices_ = device_->CreateBuffer(
        static_cast<u64>(kClusterCount) * kMaxLightsPerCluster * sizeof(u32),
        kBufferUsageStorage);
    decal_cluster_indices_ = device_->CreateBuffer(
        static_cast<u64>(kClusterCount) * kMaxDecalsPerCluster * sizeof(u32),
        kBufferUsageStorage);
    if (!cluster_counts_ || !cluster_indices_ || !decal_cluster_indices_)
      return false;
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
  hdr_pipeline_ = device_->CreateComputePipeline({
      .shader = RX_SHADER(k_hdr_capture_cs_hlsl),
      .sets = {{.slots = {{0, BindingType::kStorageBuffer},
                          {1, BindingType::kSampledImage}}}},
      .push_constant_size = 2 * sizeof(u32),
      .debug_name = "hdr_capture",
  });
  if (!hdr_pipeline_)
    return false;

  if (!local_shadows_.Initialize(*device_))
    return false; // clustered light shadows
  if (!shadow_.Initialize(*device_, material_system_->set_layout(),
                          local_shadows_.atlas().format))
    return false; // raster sun and local-shadow pipelines
  if (!froxel_fog_.Initialize(*device_)) {
    RX_WARN("froxel volumetrics unavailable"); // non-fatal: feature gates on
                                               // available()
  }
  vrs_.Initialize(*device_); // non-fatal: needs attachment VRS hardware
  if (rt_available_)
    restir_di_.Initialize(*device_);     // non-fatal: gates on available()
  virtual_texture_.Initialize(*device_); // non-fatal: gates on available()
  if (!particles_.Initialize(*device_, kSceneColorFormat,
                             bindless_ ? bindless_->set_layout()
                                       : BindingLayoutHandle{}))
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
        device_->CreateImage2D(Format::kR32Float, {1, 1}, kTextureUsageSampled);
    device_->ImmediateSubmit([&](CommandList &cmd) {
      cmd.Barrier(Transition(ms_dummy_hiz_, ResourceState::kUndefined,
                             ResourceState::kShaderReadAll));
    });
  }
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
      bindless_ ? bindless_->set_layout() : BindingLayoutHandle{});
  if (!fluid_surface_)
    RX_WARN("fluid surface renderer unavailable"); // optional feature stays off
  if (!environment_->CreateSkyPipeline(mesh_pipeline_->set_layout(),
                                       kSceneColorFormat, kMotionFormat,
                                       kDepthFormat)) {
    return false;
  }

  if (settings_.upscaler != UpscalerKind::kNone &&
      !CreateUpscalerForSettings()) {
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
    std::string value = spec;
    size_t colon = value.find_last_of(':');
    if (colon != std::string::npos) {
      screenshot_at_ = std::atof(value.c_str() + colon + 1);
      value.resize(colon);
    }
    screenshot_path_ = value;
  }

  // RX_SEQ=prefix:startsec:count[:stride] dumps a burst of composited frames.
  if (const char *spec = Sequence.get()) {
    std::string value = spec;
    base::Vector<std::string> fields;
    size_t start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
      if (i == value.size() || value[i] == ':') {
        fields.push_back(value.substr(start, i - start));
        start = i + 1;
      }
    }
    if (fields.size() >= 3) {
      seq_prefix_ = fields[0];
      seq_at_ = std::atof(fields[1].c_str());
      seq_count_ = std::atoi(fields[2].c_str());
      seq_stride_ =
          fields.size() >= 4 ? std::max(1, std::atoi(fields[3].c_str())) : 1;
    } else {
      RX_WARN(
          "RX_SEQ ignored, expected prefix:startsec:count[:stride], got '{}'",
          spec);
    }
  }

  // RX_HDR=/tmp/frame.hdr:12 exports the linear-hdr frame (radiance rgbe) at
  // t=12s.
  if (const char *spec = Hdr.get()) {
    std::string value = spec;
    size_t colon = value.find_last_of(':');
    if (colon != std::string::npos) {
      hdr_at_ = std::atof(value.c_str() + colon + 1);
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
    if (std::sscanf(sd, "%f,%f,%f", &d.x, &d.y, &d.z) == 3) {
      f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
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
    settings_.path_trace_spp = static_cast<u32>(std::max(1, int(PathtraceSpp)));
  if (PathtraceAccum.overridden())
    settings_.path_trace_accum =
        static_cast<u32>(std::max(1, int(PathtraceAccum)));
  if (PathtraceRecon.overridden())
    settings_.path_trace_recon = PathtraceRecon;
  if (PathtraceReconDebug.overridden())
    settings_.path_trace_recon_debug =
        static_cast<u32>(std::max(0, int(PathtraceReconDebug)));
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

  // RCGI software mode: on a device without ray query (or when RX_RCGI_SW
  // forces it for A/B on RT hardware), RCGI's world side runs through the SDF
  // clipmap tracer, which needs the SDF infrastructure. SDF availability is an
  // IMMUTABLE STARTUP decision, NOT a live settings bit: the CPU mesh
  // positions/indices used to voxelise mesh SDFs are not retained after upload,
  // so there is no way to backfill the field on a later toggle, and a quality
  // preset applied live must not be able to turn a seeded path off. Decide
  // want_sdf once here from the startup desc flag, the SDF-implying envs
  // (RX_SDF / RX_RCGI_SW), or a non-RT RCGI request; it is gated on creation
  // success into `sdf_available_` below and stays fixed for the session.
  // RCGI-software therefore implies the SDF memory + compose cost documented in
  // SDF_TRACE.md. (A late programmatic rcgi enable on a non-RT device that
  // seeded nothing at startup gets no software path; ApplySettings logs that
  // once.)
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

  auto t_batch1 = std::chrono::steady_clock::now();
  if (!pipeline_batch.End()) {
    RX_ERROR("pipeline batch reported failed compilations");
    return false;
  }
  auto t_batch2 = std::chrono::steady_clock::now();
  RX_INFO(
      "renderer init: {} ms (pipeline batch joined in {} ms)",
      std::chrono::duration_cast<std::chrono::milliseconds>(t_batch1 - t_batch0)
          .count(),
      std::chrono::duration_cast<std::chrono::milliseconds>(t_batch2 - t_batch1)
          .count());

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
  // EndPipelineBatch -- a failed SDF pipeline would then abort the whole
  // renderer above instead of degrading. Built immediately here, a pipeline (or
  // 3D-storage -image) failure surfaces at this call site and is handled
  // non-fatally: log, tear the SDF systems down, leave the software path
  // unavailable. (RcgiSystem's _sw pipelines are likewise created outside any
  // batch -- lazily in ApplySettings during RenderFrame -- so a failure there
  // returns a null system and is handled non-fatally at that call site too.)
  if (want_sdf) {
    sdf_scene_ = std::make_unique<SdfScene>(*device_);
    sdf_clipmap_ = std::make_unique<SdfClipmap>(*device_);
    if (!sdf_clipmap_->Initialize()) {
      RX_WARN("sdf: clipmap unavailable, disabling the SDF path");
      sdf_clipmap_.reset();
      sdf_scene_.reset();
    } else {
      sdf_available_ = true; // immutable for the session once creation succeeds
    }
  }
  return true;
}

void Renderer::CaptureScreenshot(const std::string &path) {
  screenshot_path_ = path;
  screenshot_at_ = -1;
}

void Renderer::WriteHdr() {
  device_->WaitIdle(); // the capture compute wrote hdr_readback_; drain before
                       // reading
  const f32 *src = static_cast<const f32 *>(hdr_readback_.mapped);
  if (!src) {
    hdr_path_.clear();
    return;
  }
  base::Vector<f32> rgb(static_cast<size_t>(hdr_width_) * hdr_height_ * 3);
  for (size_t i = 0; i < static_cast<size_t>(hdr_width_) * hdr_height_; ++i) {
    rgb[i * 3 + 0] = src[i * 4 + 0];
    rgb[i * 3 + 1] = src[i * 4 + 1];
    rgb[i * 3 + 2] = src[i * 4 + 2];
  }
  // .exr (OpenEXR float) is the production container; .hdr (radiance rgbe) is
  // the default. Both store the same linear pre-tonemap scene.
  bool is_exr = hdr_path_.size() >= 4 &&
                hdr_path_.compare(hdr_path_.size() - 4, 4, ".exr") == 0;
  bool ok =
      is_exr
          ? WriteExrRgbF32(hdr_path_, hdr_width_, hdr_height_, rgb.data())
          : stbi_write_hdr(hdr_path_.c_str(), static_cast<int>(hdr_width_),
                           static_cast<int>(hdr_height_), 3, rgb.data()) != 0;
  if (ok) {
    RX_INFO("{} frame written: {} ({}x{})", is_exr ? "exr" : "hdr", hdr_path_,
            hdr_width_, hdr_height_);
  } else {
    RX_WARN("hdr write failed: {}", hdr_path_);
  }
  hdr_path_.clear();
}

// Debug readback for frame generation verification (RX_FRAMEGEN_DUMP): the
// interpolated image should land between the two real frames around it.
void Renderer::DumpFgImage(const GpuImage &image, ResourceState state,
                           bool bgra, const char *path) {
  device_->WaitIdle();
  u64 size = static_cast<u64>(image.extent.width) * image.extent.height * 4;
  GpuBuffer staging =
      device_->CreateBuffer(size, kBufferUsageTransferDst, true);
  if (!staging.mapped)
    return;
  device_->ImmediateSubmit([&](CommandList &cmd) {
    cmd.Barrier(Transition(image, state, ResourceState::kCopySrc));
    cmd.CopyTextureToBuffer(image, staging, {});
    cmd.Barrier(Transition(image, ResourceState::kCopySrc, state));
  });
  base::Vector<u8> pixels(static_cast<size_t>(image.extent.width) *
                          image.extent.height * 3);
  const u8 *src = static_cast<const u8 *>(staging.mapped);
  for (size_t i = 0;
       i < static_cast<size_t>(image.extent.width) * image.extent.height; ++i) {
    pixels[i * 3 + 0] = src[i * 4 + (bgra ? 2 : 0)];
    pixels[i * 3 + 1] = src[i * 4 + 1];
    pixels[i * 3 + 2] = src[i * 4 + (bgra ? 0 : 2)];
  }
  device_->DestroyBuffer(staging);
  if (stbi_write_png(path, static_cast<int>(image.extent.width),
                     static_cast<int>(image.extent.height), 3, pixels.data(),
                     static_cast<int>(image.extent.width) * 3)) {
    RX_INFO("framegen dump written: {}", path);
  }
}

void Renderer::WriteBackbufferPng(const std::string &path, u32 image_index) {
  device_->WaitIdle();
  Extent2D extent = swapchain_->extent();
  u64 size = static_cast<u64>(extent.width) * extent.height * 4;
  GpuBuffer staging =
      device_->CreateBuffer(size, kBufferUsageTransferDst, true);
  if (!staging.mapped)
    return;

  const GpuImage &backbuffer = swapchain_->image(image_index);
  device_->ImmediateSubmit([&](CommandList &cmd) {
    cmd.Barrier(Transition(backbuffer, ResourceState::kPresent,
                           ResourceState::kCopySrc));
    cmd.CopyTextureToBuffer(backbuffer, staging, {});
    cmd.Barrier(Transition(backbuffer, ResourceState::kCopySrc,
                           ResourceState::kPresent));
  });

  // Swapchain is bgra; png wants rgb.
  base::Vector<u8> pixels(static_cast<size_t>(extent.width) * extent.height *
                          3);
  const u8 *src = static_cast<const u8 *>(staging.mapped);
  for (size_t i = 0; i < static_cast<size_t>(extent.width) * extent.height;
       ++i) {
    pixels[i * 3 + 0] = src[i * 4 + 2];
    pixels[i * 3 + 1] = src[i * 4 + 1];
    pixels[i * 3 + 2] = src[i * 4 + 0];
  }
  device_->DestroyBuffer(staging);
  if (stbi_write_png(path.c_str(), static_cast<int>(extent.width),
                     static_cast<int>(extent.height), 3, pixels.data(),
                     static_cast<int>(extent.width) * 3)) {
    RX_INFO("screenshot written: {}", path);
  } else {
    RX_WARN("screenshot write failed: {}", path);
  }
}

void Renderer::WriteScreenshot(u32 image_index) {
  WriteBackbufferPng(screenshot_path_, image_index);
  screenshot_path_.clear();
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

void Renderer::UpdateRenderResolution() {
  if (upscaler_ && settings_.aa_mode == AntiAliasingMode::kUpscaler) {
    f32 scale = UpscalerScale(settings_.upscaler_quality);
    render_width_ = static_cast<u32>(static_cast<f32>(output_width_) / scale);
    render_height_ = static_cast<u32>(static_cast<f32>(output_height_) / scale);
  } else {
    // No upscaler: render at output * render_scale. >1 supersamples (the post
    // pass samples this image into the swapchain, so it downscales for free).
    // Dynamic resolution multiplies in as a <=1 factor while active.
    f32 rs = std::clamp(settings_.render_scale * drs_.scale(), 0.25f, 2.0f);
    render_width_ =
        std::max(1u, static_cast<u32>(static_cast<f32>(output_width_) * rs));
    render_height_ =
        std::max(1u, static_cast<u32>(static_cast<f32>(output_height_) * rs));
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
        bindless_ ? bindless_->set_layout() : BindingLayoutHandle{}, want_msaa);
    if (rebuilt) {
      mesh_pipeline_ = std::move(rebuilt);
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
    upscaler_.reset();
    if (settings_.upscaler != UpscalerKind::kNone) {
      if (!CreateUpscalerForSettings()) {
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
       .sample_count = std::clamp(settings_.ao_rays * 8u, 4u, 32u)});
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
  // (SDF availability is a startup decision -- see Initialize / SDF_TRACE.md),
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
      rcgi_.reset();
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
    environment_dirty_ = true;
  }
}

void Renderer::UploadMeshletMesh(const asset::Mesh &mesh) {
  if (!device_ || device_->is_stub())
    return;
  meshlet_.Upload(*device_, mesh);
}

void Renderer::UploadVirtualGeometryMesh(const asset::Mesh &mesh) {
  if (!device_ || device_->is_stub())
    return;
  vgeo_.Upload(*device_, mesh);
}

void Renderer::SetVirtualGeometryInstances(std::span<const Mat4> transforms) {
  vgeo_.SetInstances(transforms);
}

void Renderer::SetInteriorVolumes(std::span<const InteriorVolume> volumes) {
  interior_volumes_.assign(volumes.begin(), volumes.end());
}

void Renderer::SetVirtualGeometryAlbedo(ByteSpan rgba_mips, u32 size,
                                        f32 world_to_uv) {
  if (!device_ || device_->is_stub())
    return;
  vgeo_.SetAlbedo(*device_, rgba_mips, size, world_to_uv);
}

void Renderer::SeedHairStrands(const Vec3 &head_center, f32 head_radius,
                               u32 strands, f32 length) {
  if (!device_ || device_->is_stub())
    return;
  hair_.SeedCap(*device_, head_center, head_radius, strands, length);
}

u32 Renderer::CreateHairGroom(const asset::Mesh &hair_mesh,
                              const GroomParams &params,
                              const Mat4 &transform) {
  if (!device_ || device_->is_stub())
    return 0;
  GroomData data;
  if (!BuildHairGroom(hair_mesh, params, &data)) {
    RX_WARN("hair groom build failed for mesh");
    return 0;
  }
  return hair_.CreateGroom(*device_, data, params, transform);
}

u32 Renderer::CreateHairGroom(const GroomData &data, const GroomParams &params,
                              const Mat4 &transform) {
  if (!device_ || device_->is_stub())
    return 0;
  return hair_.CreateGroom(*device_, data, params, transform);
}

void Renderer::SetHairGroomTransform(u32 id, const Mat4 &transform) {
  hair_.SetGroomTransform(id, transform);
}

void Renderer::SetHairGroomPoints(u32 id, const f32 *positions, u32 count) {
  hair_.SetGroomPoints(id, positions, count);
}

void Renderer::SetHairGroomTint(u32 id, const Vec3 &tint) {
  hair_.SetGroomTint(id, tint);
}

void Renderer::DestroyHairGroom(u32 id) {
  if (!device_ || device_->is_stub())
    return;
  hair_.DestroyGroom(*device_, id);
}

bool Renderer::HairGroomHead(u32 id, Vec3 *center, f32 *radius) {
  return hair_.GroomHead(id, center, radius);
}

void Renderer::BakeImposter(const asset::Mesh &mesh,
                            std::span<const ImposterPass::Instance> instances) {
  if (!device_ || device_->is_stub())
    return;
  imposters_.Bake(*device_, mesh);
  imposters_.SetInstances(*device_, instances);
}

InstanceGroupHandle
Renderer::CreateInstanceGroup(u64 mesh, std::span<const Mat4> transforms) {
  if (!device_ || device_->is_stub())
    return {};
  const GpuMesh *gpu = meshes_.find(mesh);
  if (!gpu || !SupportsStaticInstances(*gpu, material_system_.get()) ||
      mesh_emitters_.find(mesh) || !HasUniformScale(transforms))
    return {};
  InstanceGroupHandle handle = instances_.Create(
      *device_, mesh, transforms, gpu->bounds_center, gpu->bounds_radius);
  if (handle)
    ++scene_revision_;
  return handle;
}

bool Renderer::UpdateInstanceGroup(InstanceGroupHandle handle,
                                   std::span<const Mat4> transforms) {
  if (!device_ || device_->is_stub() ||
      handle.index >= instances_.groups().size())
    return false;
  const InstanceStore::Group &group = instances_.groups()[handle.index];
  const GpuMesh *gpu = meshes_.find(group.mesh);
  const bool replaced =
      gpu && HasUniformScale(transforms) &&
      instances_.Replace(*device_, handle, transforms, gpu->bounds_center,
                         gpu->bounds_radius);
  if (replaced)
    ++scene_revision_;
  return replaced;
}

void Renderer::DestroyInstanceGroup(InstanceGroupHandle handle) {
  if (!device_ || device_->is_stub())
    return;
  if (instances_.Destroy(*device_, handle))
    ++scene_revision_;
}

bool Renderer::UploadMesh(const asset::Mesh &mesh, u64 id_salt) {
  if (!device_ || device_->is_stub())
    return false;
  const u64 mesh_key = mesh.id.hash ^ id_salt;
  // Resolve each emitter's texture asset hash to a bindless index (or invalid)
  // so the particle billboards can sample the authored effect texture.
  auto register_emitters =
      [&](const base::Vector<asset::ParticleEmitter> &src) {
        base::Vector<asset::ParticleEmitter> emitters = src;
        for (asset::ParticleEmitter &e : emitters) {
          u32 index = BindlessRegistry::kInvalidIndex;
          if (e.texture != 0 && material_system_) {
            // The index is baked here for the mesh's lifetime; streaming would
            // move the slot under it.
            material_system_->Pin(e.texture ^ id_salt);
            index = material_system_->bindless_texture(e.texture ^ id_salt);
          }
          e.texture = index; // now a bindless index, 0xffffffff = untextured
        }
        mesh_emitters_[mesh_key] = std::move(emitters);
      };
  // Emitter-only NIFs (smoke columns, dust wisps) carry no geometry but still
  // need their particle pools: register the emitters and accept the upload so
  // the placed reference spawns and the sim runs, without a GPU mesh.
  if (mesh.lods.empty() || mesh.lods[0].vertices.empty()) {
    if (mesh.emitters.empty())
      return false;
    register_emitters(mesh.emitters);
    return true;
  }

  // kBufferUsageStorage: the bindless registry mirrors RT mesh buffers into
  // its geometry buffer array (raw reads for the DXIL hit shaders), so the
  // buffers must be descriptor-bindable, not just address-reachable.
  BufferUsageFlags rt_usage =
      raytracing_ ? (kBufferUsageAccelBuildInput | kBufferUsageDeviceAddress |
                     kBufferUsageStorage)
                  : 0;
  // The mesh-shader path reads vertices/meshlets by device address. Skinned
  // and morphed meshes stay on the raster vertex path, which deforms them.
  const bool build_meshlets = device_->caps().mesh_shaders &&
                              !mesh.dynamic_vertices && !mesh.skinned &&
                              mesh.morph_targets.empty();
  BufferUsageFlags ms_usage = build_meshlets ? kBufferUsageDeviceAddress : 0;

  // On the mesh-shader path, synthesize coarse lods for eligible
  // single-material statics so the task stage can drop detail with distance
  // (GenerateLods is a no-op for skinned / multi-submesh / already-multi-lod /
  // tiny meshes). The copy only happens for meshes that will actually get lods.
  asset::Mesh lodded;
  const asset::Mesh *src = &mesh;
  if (!mesh.dynamic_vertices && build_meshlets && mesh.lods.size() == 1 &&
      mesh.lods[0].submeshes.size() <= 1 &&
      mesh.lods[0].indices.size() >= 3000) {
    lodded = mesh;
    asset::GenerateLods(&lodded);
    if (lodded.lods.size() > 1)
      src = &lodded;
  }

  const asset::MeshLod &lod = src->lods[0];
  GpuMesh gpu;

  // Concatenate every lod into shared vertex/index buffers; each lod keeps its
  // local indices, rebased onto its vertices through the draw's vertexOffset.
  base::Vector<asset::Vertex> all_verts;
  base::Vector<u32> all_indices;
  base::Vector<u32> vertex_bases, index_bases;
  for (const asset::MeshLod &l : src->lods) {
    vertex_bases.push_back(static_cast<u32>(all_verts.size()));
    index_bases.push_back(static_cast<u32>(all_indices.size()));
    for (const asset::Vertex &v : l.vertices)
      all_verts.push_back(v);
    for (u32 idx : l.indices)
      all_indices.push_back(idx);
  }
  gpu.vertices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8 *>(all_verts.data()),
               all_verts.size() * sizeof(asset::Vertex)),
      kBufferUsageVertex | rt_usage | ms_usage);
  gpu.indices = device_->CreateBufferWithData(
      ByteSpan(reinterpret_cast<const u8 *>(all_indices.data()),
               all_indices.size() * sizeof(u32)),
      kBufferUsageIndex | rt_usage);
  gpu.index_count =
      static_cast<u32>(lod.indices.size()); // lod 0 (rt/shadow/overdraw)
  gpu.vertex_count = static_cast<u32>(lod.vertices.size());
  // Skinned meshes carry a parallel bone index/weight stream, bound as a second
  // vertex buffer by the skinned pipeline. Skinned meshes are not lod'd.
  if (mesh.skinned && lod.skinning.size() == lod.vertices.size()) {
    gpu.skinning = device_->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8 *>(lod.skinning.data()),
                 lod.skinning.size() * sizeof(asset::SkinnedVertexExtra)),
        kBufferUsageVertex);
    gpu.skinned = static_cast<bool>(gpu.skinning);
  }
  // Morph target deltas, packed [target][vertex] as {position, normal,
  // tangent} float3 triples and read by device address in the vertex shaders.
  // Targets are authored against lod 0; morphed meshes stay on it (see the
  // cull build below).
  if (!mesh.morph_targets.empty()) {
    const size_t verts = lod.vertices.size();
    base::Vector<f32> deltas(mesh.morph_targets.size() * verts * 9);
    for (size_t t = 0; t < mesh.morph_targets.size(); ++t) {
      const asset::MorphTarget &target = mesh.morph_targets[t];
      f32 *out = deltas.data() + t * verts * 9;
      for (size_t v = 0; v < verts; ++v, out += 9) {
        if (target.position_deltas.size() >= (v + 1) * 3) {
          std::memcpy(out, &target.position_deltas[v * 3], sizeof(f32) * 3);
        }
        if (target.normal_deltas.size() >= (v + 1) * 3) {
          std::memcpy(out + 3, &target.normal_deltas[v * 3], sizeof(f32) * 3);
        }
        if (target.tangent_deltas.size() >= (v + 1) * 3) {
          std::memcpy(out + 6, &target.tangent_deltas[v * 3], sizeof(f32) * 3);
        }
      }
    }
    gpu.morph_deltas = device_->CreateBufferWithData(
        ByteSpan(reinterpret_cast<const u8 *>(deltas.data()),
                 deltas.size() * sizeof(f32)),
        kBufferUsageStorage | kBufferUsageDeviceAddress);
    gpu.morph_target_count = static_cast<u32>(mesh.morph_targets.size());
  }
  auto build_submeshes = [&](const asset::MeshLod &l, u32 index_base,
                             base::Vector<GpuSubmesh> &out) {
    if (l.submeshes.empty()) {
      out.push_back(
          {index_base, static_cast<u32>(l.indices.size()), 0, false, false});
      return;
    }
    for (const asset::Submesh &submesh : l.submeshes) {
      // Store the salted material hash so every later draw-loop lookup matches
      // this domain's materials (uploaded under the same salt).
      u64 material = submesh.material.hash ^ id_salt;
      bool water = material_system_ && material_system_->is_water(material);
      bool blend =
          water || (material_system_ && material_system_->is_blend(material));
      bool mask = material_system_ && material_system_->is_mask(material);
      bool effect = material_system_ && material_system_->is_effect(material);
      bool effect_additive =
          effect && material_system_->is_effect_additive(material);
      GpuSubmesh out_submesh{index_base + submesh.index_offset,
                             submesh.index_count,
                             material,
                             blend,
                             water,
                             mask};
      out_submesh.effect = effect;
      out_submesh.effect_additive = effect_additive;
      out.push_back(out_submesh);
    }
  };
  build_submeshes(src->lods[0], index_bases[0], gpu.submeshes);
  for (size_t i = 1; i < src->lods.size(); ++i) {
    GpuLod glod;
    glod.vertex_offset = vertex_bases[i];
    build_submeshes(src->lods[i], index_bases[i], glod.submeshes);
    gpu.lods.push_back(std::move(glod));
  }
  gpu.all_blend = true;
  bool all_water = !gpu.submeshes.empty();
  for (const GpuSubmesh &submesh : gpu.submeshes) {
    if (!submesh.blend)
      gpu.all_blend = false;
    if (!submesh.water)
      all_water = false;
  }
  if (all_water && lod.vertices.size() >= 4) {
    f32 min_x = lod.vertices[0].position[0], max_x = min_x;
    f32 min_y = lod.vertices[0].position[1], max_y = min_y;
    f32 min_z = lod.vertices[0].position[2], max_z = min_z;
    for (const asset::Vertex &vertex : lod.vertices) {
      min_x = std::min(min_x, vertex.position[0]);
      max_x = std::max(max_x, vertex.position[0]);
      min_y = std::min(min_y, vertex.position[1]);
      max_y = std::max(max_y, vertex.position[1]);
      min_z = std::min(min_z, vertex.position[2]);
      max_z = std::max(max_z, vertex.position[2]);
    }
    const f32 horizontal_extent = std::max(max_x - min_x, max_z - min_z);
    const f32 planar_tolerance = std::max(0.02f, horizontal_extent * 1e-4f);
    gpu.planar_water =
        horizontal_extent > 1.0f && max_y - min_y <= planar_tolerance;
    if (gpu.planar_water) {
      gpu.water_bounds[0] = min_x;
      gpu.water_bounds[1] = min_z;
      gpu.water_bounds[2] = max_x;
      gpu.water_bounds[3] = max_z;
      gpu.water_height = (min_y + max_y) * 0.5f;
    }
  }
  std::memcpy(gpu.bounds_center, mesh.bounds_center, sizeof(f32) * 3);
  gpu.bounds_radius = mesh.bounds_radius;
  gpu.no_rt = mesh.exclude_from_rt;
  gpu.terrain_lod = mesh.terrain_lod;
  gpu.dynamic_vertices = mesh.dynamic_vertices;

  // Mesh-shader path: split every opaque submesh of every lod into meshlets,
  // concatenated into shared buffers. Each (lod, submesh) records its meshlet
  // range; the task stage picks a lod by distance and dispatches that range.
  // Meshlet vertex indices are rebased to absolute indices into the shared
  // (lod-concatenated) vertex buffer so the mesh shader pulls the right lod.
  if (build_meshlets) {
    base::Vector<Meshlet> all_meshlets;
    base::Vector<u32> all_mv;
    base::Vector<u32> all_mt;
    auto build_lod = [&](base::Vector<GpuSubmesh> &subs, u32 vertex_base,
                         u32 vertex_count) {
      for (GpuSubmesh &submesh : subs) {
        if (submesh.blend || submesh.index_count == 0)
          continue;
        MeshletGeometry geo = BuildMeshletGeometry(
            all_verts.data() + vertex_base, vertex_count,
            all_indices.data() + submesh.index_offset, submesh.index_count);
        if (geo.meshlets.empty())
          continue;
        u32 vtx_base = static_cast<u32>(all_mv.size());
        u32 tri_base = static_cast<u32>(all_mt.size());
        submesh.meshlet_offset = static_cast<u32>(all_meshlets.size());
        submesh.meshlet_count = static_cast<u32>(geo.meshlets.size());
        for (Meshlet m : geo.meshlets) {
          m.vertex_offset += vtx_base;
          m.triangle_offset += tri_base;
          all_meshlets.push_back(m);
        }
        for (u32 v : geo.vertex_indices)
          all_mv.push_back(v + vertex_base); // -> global index
        for (u32 t : geo.triangles)
          all_mt.push_back(t);
      }
    };
    build_lod(gpu.submeshes, vertex_bases[0],
              static_cast<u32>(src->lods[0].vertices.size()));
    for (size_t i = 1; i < src->lods.size(); ++i) {
      build_lod(gpu.lods[i - 1].submeshes, vertex_bases[i],
                static_cast<u32>(src->lods[i].vertices.size()));
    }
    if (!all_meshlets.empty()) {
      gpu.meshlets = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(all_meshlets.data()),
                   all_meshlets.size() * sizeof(Meshlet)),
          ms_usage);
      gpu.meshlet_vertices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(all_mv.data()),
                   all_mv.size() * sizeof(u32)),
          ms_usage);
      gpu.meshlet_triangles = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(all_mt.data()),
                   all_mt.size() * sizeof(u32)),
          ms_usage);
      gpu.has_meshlets = true;
    }
  }
  // Grass-like fill geometry stays out of the realtime tlas (bloat + noise),
  // but the path tracer wants it (alpha-tested foliage), so include it when
  // path tracing is enabled. Dynamic geometry rebuilds its BLAS and bindless
  // vertex address whenever its same-topology vertex buffer is replaced.
  bool include_rt = !gpu.no_rt || settings_.path_trace;
  // raytracing_ gate: without it the vertex/index buffers were created
  // without device-address usage, so the mesh records would hold null
  // addresses (the registry itself now always exists for textures/materials).
  if (bindless_ && raytracing_ && !gpu.all_blend && include_rt) {
    base::Vector<BindlessRegistry::GeometryRecord> geometries;
    for (const GpuSubmesh &submesh : gpu.submeshes) {
      if (submesh.blend || submesh.index_count == 0)
        continue;
      geometries.push_back(
          {submesh.index_offset,
           material_system_->bindless_material(submesh.material)});
    }
    const u32 bindless_index =
        bindless_->RegisterMesh(gpu.vertices, gpu.indices, geometries.data(),
                                static_cast<u32>(geometries.size()));
    if (bindless_index != BindlessRegistry::kInvalidIndex) {
      gpu.bindless_index = bindless_index;
      gpu.bindless_geometry = true;
    } else {
      rt_geometry_dirty_ = true;
    }
  }
  // Distance-LOD ray-tracing geometry (RX_RT_LOD_NEAR). For each extra LOD,
  // build an index buffer whose values are rebased to absolute indices into the
  // shared (all-lods-concatenated) vertex buffer, plus a bindless record, so a
  // TLAS instance can point at a coarser LOD past the near radius. Every
  // non-blend submesh is force-OPAQUE here: past the LOD switch the opaque-
  // approximation shrink is imperceptible, so distant masked foliage stays a
  // solid stand-in (no separate approx variant, matching the RAY_FLAG_CULL_NON_
  // OPAQUE realtime paths which would otherwise skip non-opaque geometry). The
  // BLAS is deferred to EnsureLodRtGeometry (first selection); only the cheap
  // index buffer + record are made here, where the CPU geometry is in hand.
  if (bindless_ && raytracing_ && !gpu.all_blend && include_rt &&
      !gpu.lods.empty()) {
    const u32 total_verts = static_cast<u32>(all_verts.size());
    gpu.lod_rt.resize(gpu.lods.size());
    for (size_t li = 0; li < gpu.lods.size(); ++li) {
      const u32 vertex_base = vertex_bases[li + 1]; // vertex_bases[0] = lod0
      base::Vector<u32> lod_indices;
      base::Vector<BindlessRegistry::GeometryRecord> lod_geoms;
      GpuMesh::LodRt &rt = gpu.lod_rt[li];
      for (const GpuSubmesh &submesh : gpu.lods[li].submeshes) {
        if (submesh.blend || submesh.index_count == 0)
          continue;
        const u32 offset = static_cast<u32>(lod_indices.size());
        for (u32 e = 0; e < submesh.index_count; ++e)
          lod_indices.push_back(all_indices[submesh.index_offset + e] +
                                vertex_base);
        rt.geoms.push_back({offset, submesh.index_count});
        lod_geoms.push_back(
            {offset, material_system_->bindless_material(submesh.material)});
      }
      if (lod_indices.empty())
        continue;
      rt.vertex_count = total_verts;
      rt.indices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(lod_indices.data()),
                   lod_indices.size() * sizeof(u32)),
          kBufferUsageIndex | kBufferUsageAccelBuildInput |
              kBufferUsageDeviceAddress | kBufferUsageStorage);
      if (!rt.indices) {
        rt.geoms.clear();
        continue;
      }
      rt.bindless =
          bindless_->RegisterMesh(gpu.vertices, rt.indices, lod_geoms.data(),
                                  static_cast<u32>(lod_geoms.size()));
    }
  }
  // Opaque-approximation variant for alpha-masked (vegetation) submeshes. Each
  // masked triangle is duplicated with its own three vertices, shrunk about its
  // centroid by sqrt(average opacity) so the stand-in's area matches the
  // average covered fraction, and flagged OPAQUE. Realtime diffuse GI / AO /
  // shadow rays hit this via kRayMaskApprox and skip the real (non-opaque)
  // masked geometry, which the path tracer and reflections keep. RX_RT_VEG=0
  // forces the shrink factor to 1 (identical to the real triangles),
  // reproducing today's force-opaque behavior for A/B. Realtime-tlas meshes
  // only (no_rt fill is path-trace-only and never hits realtime rays).
  base::Vector<AccelTriangles> approx_accel;
  if (bindless_ && raytracing_ && material_system_ && !gpu.all_blend &&
      !gpu.no_rt) {
    const bool veg = RtVegOpt;
    base::Vector<asset::Vertex> approx_verts;
    base::Vector<u32> approx_indices;
    base::Vector<BindlessRegistry::GeometryRecord> approx_geoms;
    struct ApproxRange {
      u32 index_offset;
      u32 index_count;
    };
    base::Vector<ApproxRange> approx_ranges;
    for (const GpuSubmesh &submesh : gpu.submeshes) {
      if (!submesh.alpha_mask || submesh.blend || submesh.index_count == 0)
        continue;
      f32 opacity =
          veg ? MaskedSubmeshOpacity(
                    all_verts, all_indices, submesh,
                    material_system_->material_base_alpha(submesh.material))
              : 1.0f;
      const f32 s = std::sqrt(std::clamp(opacity, 0.02f, 1.0f));
      const u32 base_index = static_cast<u32>(approx_indices.size());
      for (u32 e = 0; e + 3 <= submesh.index_count; e += 3) {
        const u32 idx[3] = {all_indices[submesh.index_offset + e],
                            all_indices[submesh.index_offset + e + 1],
                            all_indices[submesh.index_offset + e + 2]};
        if (idx[0] >= all_verts.size() || idx[1] >= all_verts.size() ||
            idx[2] >= all_verts.size())
          continue;
        f32 cen[3] = {0, 0, 0};
        for (u32 k = 0; k < 3; ++k)
          for (u32 c = 0; c < 3; ++c)
            cen[c] += all_verts[idx[k]].position[c] / 3.0f;
        for (u32 k = 0; k < 3; ++k) {
          asset::Vertex v =
              all_verts[idx[k]]; // keep normal/tangent/uv/color for hit shading
          for (u32 c = 0; c < 3; ++c)
            v.position[c] = cen[c] + s * (v.position[c] - cen[c]);
          approx_indices.push_back(static_cast<u32>(approx_verts.size()));
          approx_verts.push_back(v);
        }
      }
      const u32 count = static_cast<u32>(approx_indices.size()) - base_index;
      if (count == 0)
        continue;
      approx_geoms.push_back(
          {base_index, material_system_->bindless_material(submesh.material)});
      approx_ranges.push_back({base_index, count});
    }
    if (!approx_verts.empty()) {
      BufferUsageFlags approx_usage = kBufferUsageAccelBuildInput |
                                      kBufferUsageDeviceAddress |
                                      kBufferUsageStorage;
      gpu.rt_approx_vertices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(approx_verts.data()),
                   approx_verts.size() * sizeof(asset::Vertex)),
          kBufferUsageVertex | approx_usage);
      gpu.rt_approx_indices = device_->CreateBufferWithData(
          ByteSpan(reinterpret_cast<const u8 *>(approx_indices.data()),
                   approx_indices.size() * sizeof(u32)),
          kBufferUsageIndex | approx_usage);
      if (gpu.rt_approx_vertices && gpu.rt_approx_indices) {
        gpu.rt_approx_bindless = bindless_->RegisterMesh(
            gpu.rt_approx_vertices, gpu.rt_approx_indices, approx_geoms.data(),
            static_cast<u32>(approx_geoms.size()));
        gpu.rt_approx_bindless_valid =
            gpu.rt_approx_bindless != BindlessRegistry::kInvalidIndex;
        if (!gpu.rt_approx_bindless_valid)
          gpu.rt_approx_bindless = 0;
        for (const ApproxRange &r : approx_ranges) {
          approx_accel.push_back(
              {.vertex_address = gpu.rt_approx_vertices.address,
               .vertex_stride = sizeof(asset::Vertex),
               .vertex_count = static_cast<u32>(approx_verts.size()),
               .vertex_format = Format::kRGB32Float,
               .index_address =
                   gpu.rt_approx_indices.address + r.index_offset * sizeof(u32),
               .index_count = r.index_count,
               .index_type = IndexType::kUint32,
               .opaque = true});
        }
        gpu.rt_approx = !approx_accel.empty();
      }
    }
  }
  // Re-uploading under an existing key (the builtin biped goes up once for
  // the test spawn and again for the npc template) must free the previous
  // buffers or they leak until vkDestroyDevice complains.
  const bool replacing_mesh = meshes_.find(mesh_key) != nullptr;
  if (GpuMesh *previous = meshes_.find(mesh_key)) {
    device_->WaitIdle(); // uploads happen at load time; never per frame
    if (raytracing_) {
      raytracing_->RemoveBlas(mesh_key);
      raytracing_->RemoveApproxBlas(mesh_key);
      raytracing_->RemoveLodBlas(mesh_key);
    }
    if (bindless_ && previous->bindless_geometry)
      bindless_->ReleaseMesh(previous->bindless_index);
    for (GpuMesh::LodRt &rt : previous->lod_rt) {
      if (rt.indices)
        device_->DestroyBuffer(rt.indices);
      // Now that mesh-table slots recycle, the per-LOD and approx records must
      // come back too or every same-key re-upload leaks table entries.
      if (bindless_ && rt.bindless != BindlessRegistry::kInvalidIndex)
        bindless_->ReleaseMesh(rt.bindless);
    }
    if (bindless_ && previous->rt_approx_bindless_valid)
      bindless_->ReleaseMesh(previous->rt_approx_bindless);
    device_->DestroyBuffer(previous->vertices);
    device_->DestroyBuffer(previous->indices);
    if (previous->skinning)
      device_->DestroyBuffer(previous->skinning);
    if (previous->morph_deltas)
      device_->DestroyBuffer(previous->morph_deltas);
    if (previous->meshlets)
      device_->DestroyBuffer(previous->meshlets);
    if (previous->meshlet_vertices)
      device_->DestroyBuffer(previous->meshlet_vertices);
    if (previous->meshlet_triangles)
      device_->DestroyBuffer(previous->meshlet_triangles);
    if (previous->rt_approx_vertices)
      device_->DestroyBuffer(previous->rt_approx_vertices);
    if (previous->rt_approx_indices)
      device_->DestroyBuffer(previous->rt_approx_indices);
  }
  meshes_[mesh_key] = gpu;
  instances_.RefreshMesh(*device_, mesh_key, gpu.bounds_center,
                         gpu.bounds_radius,
                         SupportsStaticInstances(gpu, material_system_.get()) &&
                             mesh.emitters.empty());
  // NIF particle emitters ride along with the mesh; every placed draw of it
  // feeds a cpu pool (see emitter_sim_ in BuildFrameGraph).
  if (!mesh.emitters.empty())
    register_emitters(mesh.emitters);
  else
    mesh_emitters_.erase(mesh_key);
  // Pure transparency never enters the tlas: water occluding rtao and
  // shadow rays would black out everything under it.
  if (raytracing_ && !gpu.all_blend && include_rt && gpu.bindless_geometry &&
      !raytracing_->BuildBlas(mesh_key, gpu)) {
    bindless_->ReleaseMesh(gpu.bindless_index);
    gpu.bindless_index = 0;
    gpu.bindless_geometry = false;
    if (GpuMesh *stored = meshes_.find(mesh_key)) {
      stored->bindless_index = 0;
      stored->bindless_geometry = false;
    }
    rt_geometry_dirty_ = true;
  }
  // Opaque-approximation BLAS for the shrunk vegetation stand-in (see above).
  if (raytracing_ && gpu.rt_approx && !approx_accel.empty())
    raytracing_->BuildApproxBlas(mesh_key, approx_accel);
  // SDF: generate a per-mesh signed distance field from the lod-0 CPU geometry.
  // The SDF stands in for RCGI's realtime visibility rays
  // (RX_RAY_MASK_REALTIME), so it must mirror that TLAS set exactly: skip no_rt
  // fill geometry entirely (never in the realtime tlas), and build the field
  // from only the OPAQUE submesh triangle ranges (blended submeshes --
  // glass/water/effects -- are excluded from the tlas and must not turn the SDF
  // opaque). Average albedo / emissive over the opaque submeshes only, matching
  // the geometry that fed it. Eligibility can flip on a same-key re-upload
  // (opaque mesh replaced by an all- blend / no_rt one, or one that lost its
  // opaque submeshes). When it does the block below never calls RegisterMesh,
  // so the previous field must be dropped explicitly or it lingers as a stale
  // occluder -- Remove covers every such replacement path (no-op when nothing
  // was registered).
  bool sdf_eligible =
      sdf_scene_ && !gpu.all_blend && !gpu.no_rt && !gpu.dynamic_vertices;
  if (sdf_eligible) {
    SdfScene::MeshInput in{};
    in.positions = lod.vertices.empty() ? nullptr : lod.vertices[0].position;
    in.position_stride = static_cast<u32>(sizeof(asset::Vertex));
    in.vertex_count = static_cast<u32>(lod.vertices.size());
    // Concatenate the opaque submeshes' index ranges (their index_offset is
    // absolute in the shared buffer; lod 0's base is 0, so it indexes
    // lod.indices).
    base::Vector<u32> opaque_indices;
    f32 albedo[3] = {0, 0, 0}, emissive[3] = {0, 0, 0};
    u64 weight = 0;
    for (const GpuSubmesh &sm : gpu.submeshes) {
      if (sm.blend || sm.index_count == 0)
        continue;
      if (!lod.indices.empty()) {
        for (u32 e = 0; e < sm.index_count; ++e) {
          u32 gi = sm.index_offset + e;
          if (gi < lod.indices.size())
            opaque_indices.push_back(lod.indices[gi]);
        }
      }
      if (material_system_) {
        MaterialSystem::MaterialColor mc =
            material_system_->material_color(sm.material);
        u64 w = std::max<u64>(sm.index_count, 1);
        for (int k = 0; k < 3; ++k) {
          albedo[k] += mc.albedo[k] * static_cast<f32>(w);
          emissive[k] += mc.emissive[k] * static_cast<f32>(w);
        }
        weight += w;
      }
    }
    in.indices = opaque_indices.empty() ? nullptr : opaque_indices.data();
    in.index_count = static_cast<u32>(opaque_indices.size());
    if (weight > 0) {
      for (int k = 0; k < 3; ++k) {
        in.albedo[k] = albedo[k] / static_cast<f32>(weight);
        in.emissive[k] = emissive[k] / static_cast<f32>(weight);
      }
    }
    // Only register when there is opaque indexed geometry to voxelise (an all-
    // blend mesh is already excluded above; a non-indexed opaque mesh is not a
    // shape we generate SDFs for here). If there is none, this is not eligible
    // after all -- drop any prior field below.
    if (in.positions && in.index_count > 0)
      sdf_scene_->RegisterMesh(mesh_key, in);
    else
      sdf_eligible = false;
  }
  // Any ineligible replacement (or first upload) removes a stale SDF for this
  // key.
  if (sdf_scene_ && !sdf_eligible)
    sdf_scene_->Remove(mesh_key);
  // Foliage uploaded while path tracing was off got no blas/geometry above;
  // flag a catch-up so toggling path tracing on later still pulls it into the
  // tlas (BuildFrameGraph runs EnsureRayTracingGeometry on the next path-traced
  // frame).
  if (raytracing_ && !gpu.all_blend && gpu.no_rt && !include_rt)
    rt_foliage_dirty_ = true;
  if (replacing_mesh)
    ++scene_revision_;
  return true;
}

bool Renderer::UpdateDynamicMesh(const asset::Mesh &mesh, u64 id_salt) {
  if (!device_ || device_->is_stub() || !mesh.dynamic_vertices ||
      mesh.lods.size() != 1) {
    return false;
  }
  const asset::MeshLod &lod = mesh.lods[0];
  const u64 key = mesh.id.hash ^ id_salt;
  GpuMesh *gpu = meshes_.find(key);
  // rt_approx meshes are rejected: the opaque-approx stand-in duplicates the
  // masked geometry into its own buffers/BLAS, which this fast path does not
  // rebuild -- realtime rays would keep hitting the pre-edit shape. Callers
  // fall back to a full UploadMesh, which rebuilds the stand-in.
  if (!gpu || gpu->skinned || gpu->morph_target_count != 0 ||
      !gpu->lods.empty() || gpu->rt_approx ||
      lod.vertices.size() != gpu->vertex_count ||
      lod.indices.size() != gpu->index_count) {
    return false;
  }

  const ByteSpan bytes(reinterpret_cast<const u8 *>(lod.vertices.data()),
                       lod.vertices.size() * sizeof(asset::Vertex));
  const BufferUsageFlags rt_usage =
      raytracing_ ? (kBufferUsageAccelBuildInput | kBufferUsageDeviceAddress |
                     kBufferUsageStorage)
                  : 0;
  GpuBuffer replacement =
      device_->CreateBufferWithData(bytes, kBufferUsageVertex | rt_usage);
  if (!replacement)
    return false;

  if (gpu->bindless_geometry) {
    retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight].push_back(
        gpu->bindless_index);
    gpu->bindless_index = 0;
    gpu->bindless_geometry = false;
    if (gpu->no_rt)
      rt_foliage_dirty_ = true;
  }
  if (raytracing_)
    raytracing_->RemoveBlasDeferred(key);
  GpuBuffer previous = gpu->vertices;
  gpu->vertices = replacement;
  std::memcpy(gpu->bounds_center, mesh.bounds_center,
              sizeof(gpu->bounds_center));
  gpu->bounds_radius = mesh.bounds_radius;
  device_->DestroyBufferDeferred(previous);
  if (!gpu->dynamic_vertices) {
    if (gpu->meshlets)
      device_->DestroyBufferDeferred(gpu->meshlets);
    if (gpu->meshlet_vertices)
      device_->DestroyBufferDeferred(gpu->meshlet_vertices);
    if (gpu->meshlet_triangles)
      device_->DestroyBufferDeferred(gpu->meshlet_triangles);
    gpu->meshlets = {};
    gpu->meshlet_vertices = {};
    gpu->meshlet_triangles = {};
    gpu->has_meshlets = false;
    gpu->lods.clear();
    gpu->dynamic_vertices = true;
    if (sdf_scene_)
      sdf_scene_->RemoveDeferred(key);
  }
  instances_.RefreshMesh(
      *device_, mesh.id.hash ^ id_salt, gpu->bounds_center, gpu->bounds_radius,
      SupportsStaticInstances(*gpu, material_system_.get()) &&
          mesh.emitters.empty());
  ++scene_revision_;
  return true;
}

bool Renderer::SyncDynamicMeshRayTracing(const asset::Mesh &mesh, u64 id_salt) {
  if (!device_ || device_->is_stub())
    return false;
  const u64 key = mesh.id.hash ^ id_salt;
  GpuMesh *gpu = meshes_.find(key);
  if (!gpu || !gpu->dynamic_vertices)
    return false;
  if (!raytracing_ || !bindless_ || !material_system_ ||
      (gpu->no_rt && !settings_.path_trace)) {
    return true;
  }
  if (gpu->bindless_geometry && raytracing_->HasBlas(key))
    return true;

  base::Vector<BindlessRegistry::GeometryRecord> geometries;
  for (const GpuSubmesh &submesh : gpu->submeshes) {
    if (submesh.blend || submesh.index_count == 0)
      continue;
    geometries.push_back(
        {submesh.index_offset,
         material_system_->bindless_material(submesh.material)});
  }
  const u32 bindless_index =
      bindless_->RegisterMesh(gpu->vertices, gpu->indices, geometries.data(),
                              static_cast<u32>(geometries.size()));
  if (bindless_index == BindlessRegistry::kInvalidIndex) {
    rt_geometry_dirty_ = true;
    if (gpu->no_rt)
      rt_foliage_dirty_ = true;
    return false;
  }
  gpu->bindless_index = bindless_index;
  gpu->bindless_geometry = true;
  if (!raytracing_->BuildBlas(key, *gpu)) {
    bindless_->ReleaseMesh(bindless_index);
    gpu->bindless_index = 0;
    gpu->bindless_geometry = false;
    rt_geometry_dirty_ = true;
    return false;
  }
  ++scene_revision_;
  return true;
}

bool Renderer::RemoveDynamicMesh(asset::AssetId mesh, u64 id_salt) {
  if (!device_ || device_->is_stub())
    return false;
  const u64 key = mesh.hash ^ id_salt;
  GpuMesh *gpu = meshes_.find(key);
  if (!gpu || !gpu->dynamic_vertices || gpu->has_meshlets) {
    return false;
  }
  instances_.RefreshMesh(*device_, key, gpu->bounds_center, gpu->bounds_radius,
                         false);
  if (bindless_ && gpu->bindless_geometry) {
    retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight].push_back(
        gpu->bindless_index);
  }
  device_->DestroyBufferDeferred(gpu->vertices);
  device_->DestroyBufferDeferred(gpu->indices);
  if (gpu->skinning)
    device_->DestroyBufferDeferred(gpu->skinning);
  if (gpu->morph_deltas)
    device_->DestroyBufferDeferred(gpu->morph_deltas);
  // Per-LOD RT and opaque-approx side state: without these a later UploadMesh
  // under the same asset id would find (and silently reuse) the stale
  // approx/LOD BLAS entries, and the bindless records would leak for good.
  for (GpuMesh::LodRt &rt : gpu->lod_rt) {
    if (rt.indices)
      device_->DestroyBufferDeferred(rt.indices);
    if (bindless_ && rt.bindless != BindlessRegistry::kInvalidIndex) {
      retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight].push_back(
          rt.bindless);
    }
  }
  if (gpu->rt_approx_vertices)
    device_->DestroyBufferDeferred(gpu->rt_approx_vertices);
  if (gpu->rt_approx_indices)
    device_->DestroyBufferDeferred(gpu->rt_approx_indices);
  if (bindless_ && gpu->rt_approx_bindless_valid) {
    retired_bindless_meshes_[(frame_index_ + 1) % kFramesInFlight].push_back(
        gpu->rt_approx_bindless);
  }
  mesh_emitters_.erase(key);
  // A dynamic mesh can never have a registered SDF (sdf_eligible excludes
  // dynamic_vertices), but keep the frame-safe variant so that stays true by
  // construction rather than by argument.
  if (sdf_scene_)
    sdf_scene_->RemoveDeferred(key);
  if (raytracing_) {
    raytracing_->RemoveBlasDeferred(key);
    raytracing_->RemoveApproxBlasDeferred(key);
    raytracing_->RemoveLodBlasDeferred(key);
  }
  meshes_.erase(key);
  ++scene_revision_;
  return true;
}

bool Renderer::EnsureRayTracingGeometry() {
  if (!bindless_ || !raytracing_ || !material_system_)
    return false;
  bool success = true;
  for (auto entry : meshes_) {
    GpuMesh &gpu = entry.value;
    if (gpu.all_blend || (gpu.no_rt && !settings_.path_trace))
      continue;
    if (raytracing_->HasBlas(entry.key))
      continue; // already built
    base::Vector<BindlessRegistry::GeometryRecord> geometries;
    for (const GpuSubmesh &submesh : gpu.submeshes) {
      if (submesh.blend || submesh.index_count == 0)
        continue;
      geometries.push_back(
          {submesh.index_offset,
           material_system_->bindless_material(submesh.material)});
    }
    const u32 bindless_index =
        bindless_->RegisterMesh(gpu.vertices, gpu.indices, geometries.data(),
                                static_cast<u32>(geometries.size()));
    if (bindless_index == BindlessRegistry::kInvalidIndex) {
      success = false;
      continue;
    }
    gpu.bindless_index = bindless_index;
    gpu.bindless_geometry = true;
    if (!raytracing_->BuildBlas(entry.key, gpu)) {
      bindless_->ReleaseMesh(bindless_index);
      gpu.bindless_index = 0;
      gpu.bindless_geometry = false;
      success = false;
    }
  }
  return success;
}

u32 Renderer::EnsureLodRtGeometry(u64 mesh_key, GpuMesh &mesh, u32 lod) {
  if (lod == 0 || lod > mesh.lod_rt.size())
    return BindlessRegistry::kInvalidIndex;
  GpuMesh::LodRt &rt = mesh.lod_rt[lod - 1];
  if (rt.bindless == BindlessRegistry::kInvalidIndex || rt.geoms.empty())
    return BindlessRegistry::kInvalidIndex; // no RT geometry at this LOD
  if (!rt.blas_built) {
    // Reconstruct the accel geometry from the eagerly-built (absolute-indexed,
    // force-opaque) LOD index buffer and build the BLAS once. The build blocks
    // (ImmediateSubmit), but only the first time this LOD is needed.
    base::Vector<AccelTriangles> geometries;
    geometries.reserve(rt.geoms.size());
    for (const GpuMesh::LodRt::Geom &g : rt.geoms) {
      geometries.push_back(
          {.vertex_address = mesh.vertices.address,
           .vertex_stride = sizeof(asset::Vertex),
           .vertex_count = rt.vertex_count,
           .vertex_format = Format::kRGB32Float,
           .index_address = rt.indices.address + g.index_offset * sizeof(u32),
           .index_count = g.index_count,
           .index_type = IndexType::kUint32,
           .opaque = true});
    }
    if (!raytracing_->BuildLodBlas(mesh_key, lod, geometries))
      return BindlessRegistry::kInvalidIndex;
    rt.blas_built = true;
  }
  return rt.bindless;
}

void Renderer::SetDecalAtlas(asset::AssetId texture,
                             asset::AssetId normal_atlas) {
  if (!material_system_)
    return;
  // The cached views below dangle if the streamer ever swaps these images.
  material_system_->Pin(texture.hash);
  if (normal_atlas)
    material_system_->Pin(normal_atlas.hash);
  const GpuImage *img = material_system_->find_texture(texture.hash);
  decal_atlas_view_ = img ? img->view : TextureView{};
  const GpuImage *normal_img =
      normal_atlas ? material_system_->find_texture(normal_atlas.hash)
                   : nullptr;
  decal_normal_atlas_view_ = normal_img ? normal_img->view : TextureView{};
}

bool Renderer::UploadTexture(const asset::Texture &texture, u64 id_salt) {
  if (!material_system_)
    return false;
  return material_system_->UploadTexture(texture, id_salt);
}

bool Renderer::UploadMaterial(const asset::Material &material, u64 id_salt) {
  if (!material_system_)
    return false;
  return material_system_->UploadMaterial(material, id_salt);
}

void Renderer::RenderFrame(const FrameView &view) {
  static const mem::Category kRenderCategory = mem::RegisterCategory("render");
  mem::CategoryScope mem_scope(kRenderCategory);
  if (!device_ || device_->is_stub() || !swapchain_)
    return;

  // Wayland surfaces report an undefined currentExtent, so the driver never
  // flags the swapchain out-of-date on a window resize (unlike X11). Poll the
  // window each frame and recreate when it no longer matches the swapchain, so
  // the rendered output tracks the window the same way imgui's DisplaySize does
  // - otherwise the overlay is drawn/hit-tested against a stale size and clicks
  // stop landing on widgets after a resize.
  if (window_) {
    u32 w = window_->width(), h = window_->height();
    if (w != 0 && h != 0 && (w != output_width_ || h != output_height_))
      RecreateSwapchain();
    // The effective HDR request tracks the live OS state (system toggle
    // flipped, window moved to an SDR monitor, hdr_output changed): rebuild
    // when it diverges from what the current swapchain was built with.
    // Comparing against the request (not the achieved color space) avoids a
    // recreate loop when the surface cannot satisfy it.
    if (WantHdrSwapchain() != swapchain_hdr_request_)
      RecreateSwapchain();
  }

  ApplySettings();

  u32 slot = frame_index_ % kFramesInFlight;
  // Waits on the slot's fence, resets its command allocator and transient
  // descriptor pool, and begins recording.
  CommandList *cmd = device_->BeginFrame(slot);
  if (bindless_) {
    for (u32 index : retired_bindless_meshes_[slot])
      bindless_->ReleaseMesh(index);
    retired_bindless_meshes_[slot].clear();
  }

  u32 image_index = 0;
  AcquireResult acquired = swapchain_->Acquire(slot, &image_index);
  if (acquired == AcquireResult::kOutOfDate) {
    RecreateSwapchain();
    return;
  }
  if (acquired != AcquireResult::kOk && acquired != AcquireResult::kSuboptimal)
    return;

  // Frame generation: acquire a second image for the interpolated present.
  bool fg_frame = false;
  u32 interp_index = 0;
#if defined(RX_HAS_FSR3)
  if (settings_.frame_generation && !settings_.path_trace &&
      swapchain_->color_space() == ColorSpace::kSrgbNonlinear && upscaler_ &&
      upscaler_->kind() == UpscalerKind::kFsr3) {
    if (!framegen_ && !framegen_attempted_) {
      framegen_attempted_ = true;
      framegen_ = CreateFrameGenerator(
          *device_, {.display_width = swapchain_->extent().width,
                     .display_height = swapchain_->extent().height,
                     .render_width = render_width_,
                     .render_height = render_height_});
      if (!framegen_)
        RX_WARN("framegen: unavailable, presenting real frames only");
    }
    if (framegen_) {
      AcquireResult second = swapchain_->AcquireSecond(slot, &interp_index);
      fg_frame =
          second == AcquireResult::kOk || second == AcquireResult::kSuboptimal;
    }
  }
#endif

  // Texture streaming: flush the retire ring (safe now - BeginFrame waited the
  // slot fence) and run the promote/demote policy before any pass records, so
  // the whole frame binds one consistent generation of material sets.
  if (material_system_) {
    material_system_->BeginFrame(frame_index_);
    material_system_->UpdateStreaming(frame_index_);
  }

  transient_pool_->BeginFrame();
  graph_.Reset();
  fg_active_frame_ = fg_frame;
  BuildFrameGraph(frames_[slot], image_index, view);
  if (!graph_.Compile(*device_, *transient_pool_))
    return;

  profiler_.SetDetail(settings_.gpu_pass_timings);
  profiler_.BeginFrame(*cmd, slot);
  graph_.SetPassHooks(
      [this](CommandList &c, const char *name) {
        profiler_.BeginPass(c, name);
      },
      [this](CommandList &c) { profiler_.EndPass(c); });

  PassContext ctx;
  ctx.cmd = cmd;
  ctx.device = device_.get();
  ctx.graph = &graph_;
  // With per-pass detail off the whole frame gets one bracket so
  // gpu_frame_ms() (dynamic resolution's input) stays fed.
  profiler_.BeginFrameTotal(*cmd);
  // With async passes the graph splits the frame into segments; the returned
  // list is the final one and the only valid argument for SubmitFrame.
  CommandList *final_cmd = graph_.Execute(ctx);
  profiler_.EndFrameTotal(*final_cmd);

  PresentResult presented;
#if defined(RX_HAS_FSR3)
  Fsr3SharedResources fg_shared;
  if (fg_frame && upscaler_ && upscaler_->fsr3_shared(&fg_shared)) {
    const GpuImage &backbuffer = swapchain_->image(image_index);
    const GpuImage &target = swapchain_->image(interp_index);
    // The graph's final barrier left the backbuffer in PRESENT; bring it back
    // for the interpolation dispatch to sample.
    {
      TextureBarrier to_read = Transition(backbuffer, ResourceState::kPresent,
                                          ResourceState::kShaderReadCompute);
      final_cmd->TextureBarriers({&to_read, 1});
    }
    FrameGenInputs fin;
    fin.backbuffer = &backbuffer;
    fin.dilated_depth = fg_shared.dilated_depth;
    fin.dilated_motion = fg_shared.dilated_motion;
    fin.recon_prev_depth = fg_shared.recon_prev_depth;
    fin.frame_delta_seconds = view.frame_delta_seconds;
    fin.camera_near = 0.1f;
    fin.camera_fov_y = view.camera.fov_y;
    fin.frame_id = frame_index_;
    fin.reset = !framegen_was_active_;
    bool interpolated = framegen_->Record(*final_cmd, fin);
    framegen_was_active_ = interpolated;

    if (interpolated) {
      const GpuImage &interp = framegen_->interpolated();
      TextureBarrier pre[] = {
          Transition(interp, ResourceState::kGeneral, ResourceState::kCopySrc),
          Transition(target, ResourceState::kUndefined,
                     ResourceState::kCopyDst)};
      final_cmd->TextureBarriers(pre);
      final_cmd->CopyTexture(interp, target);
      TextureBarrier mid[] = {
          Transition(interp, ResourceState::kCopySrc, ResourceState::kGeneral),
          Transition(target, ResourceState::kCopyDst,
                     ResourceState::kColorTarget)};
      final_cmd->TextureBarriers(mid);
      // Re-draw the UI onto the generated frame (the interpolation sourced the
      // pre-UI copy). Both backends replay retained draw data, so recording
      // them twice per frame is safe; blur_source was filled by the ui pass.
      if (view.hud_draw || view.ui_draw) {
        ColorAttachment ui_color{.view = target.view, .load = LoadOp::kLoad};
        final_cmd->BeginRendering(
            {.extent = target.extent, .colors = {&ui_color, 1}});
        if (view.hud_draw)
          view.hud_draw(*final_cmd);
        if (view.ui_draw)
          view.ui_draw(*final_cmd);
        final_cmd->EndRendering();
      }
      TextureBarrier post[] = {Transition(target, ResourceState::kColorTarget,
                                          ResourceState::kPresent),
                               Transition(backbuffer,
                                          ResourceState::kShaderReadCompute,
                                          ResourceState::kPresent)};
      final_cmd->TextureBarriers(post);
    } else {
      // Dispatch failed: duplicate the real frame so the acquired image still
      // presents something sane.
      TextureBarrier pre[] = {Transition(backbuffer,
                                         ResourceState::kShaderReadCompute,
                                         ResourceState::kCopySrc),
                              Transition(target, ResourceState::kUndefined,
                                         ResourceState::kCopyDst)};
      final_cmd->TextureBarriers(pre);
      final_cmd->CopyTexture(backbuffer, target);
      TextureBarrier post[] = {
          Transition(backbuffer, ResourceState::kCopySrc,
                     ResourceState::kPresent),
          Transition(target, ResourceState::kCopyDst, ResourceState::kPresent)};
      final_cmd->TextureBarriers(post);
    }
    presented = device_->SubmitFrameGen(final_cmd, *swapchain_, interp_index,
                                        image_index);
    fg_presents_ += 2;

    // Debug: RX_FRAMEGEN_DUMP=<frame> writes real frame N, the interpolated
    // N->N+1 midpoint and real frame N+1 as pngs in the working directory.
    if (const char *dump = std::getenv("RX_FRAMEGEN_DUMP")) {
      u64 dump_frame = std::strtoull(dump, nullptr, 10);
      if (frame_index_ == dump_frame) {
        DumpFgImage(swapchain_->image(image_index), ResourceState::kPresent,
                    true, "fg_dump_real0.png");
      } else if (frame_index_ == dump_frame + 1) {
        DumpFgImage(framegen_->interpolated(), ResourceState::kGeneral, false,
                    "fg_dump_interp.png");
        DumpFgImage(framegen_->hudless(), ResourceState::kShaderReadCompute,
                    false, "fg_dump_hudless.png");
        DumpFgImage(swapchain_->image(image_index), ResourceState::kPresent,
                    true, "fg_dump_real1.png");
      }
    }
  } else
#endif
  {
    presented = device_->SubmitFrame(final_cmd, *swapchain_, image_index);
    framegen_was_active_ = false;
    fg_presents_ += 1;
  }
  instances_.OnFrameSubmitted(*device_);

  // Present-rate accounting: the observable proof that generation runs.
  ++fg_engine_frames_;
  if (settings_.frame_generation && time_seconds_ - fg_log_time_ >= 2.0) {
    if (fg_log_time_ > 0.0) {
      f64 span = time_seconds_ - fg_log_time_;
      RX_INFO("framegen: {:.0f} engine fps -> {:.0f} presented fps",
              fg_engine_frames_ / span, fg_presents_ / span);
    }
    fg_log_time_ = time_seconds_;
    fg_engine_frames_ = 0;
    fg_presents_ = 0;
  }

  if (!screenshot_path_.empty() && time_seconds_ >= screenshot_at_) {
    WriteScreenshot(image_index);
  }
  if (!seq_prefix_.empty() && seq_written_ < seq_count_ &&
      time_seconds_ >= seq_at_) {
    if (seq_frame_ctr_ % seq_stride_ == 0) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s_%04d.png", seq_prefix_.c_str(),
                    seq_written_);
      WriteBackbufferPng(path, image_index);
      ++seq_written_;
    }
    ++seq_frame_ctr_;
  }
  if (hdr_pending_) {
    WriteHdr();
    hdr_pending_ = false;
  }

  if (presented == PresentResult::kOutOfDate) {
    RecreateSwapchain();
  }

  // Editor picking runs as a standalone synchronous submit after the frame (a
  // rare operation, so the stall is acceptable) so it never perturbs the main
  // frame graph. It reuses the meshes/transforms this frame presented.
  if (pick_requested_)
    RenderPickPass(view);

  ++frame_index_;
}

// --- Editor debug lines ------------------------------------------------------

namespace {
// One vertex of a debug line: world position + packed rgba8 colour.
struct DebugLineVertex {
  f32 pos[3];
  u8 rgba[4];
};
struct DebugLinePush {
  Mat4 view_proj;
};
} // namespace

void Renderer::BuildDebugLinePipelines() {
  if (debug_line_pipeline_)
    return;
  VertexBufferLayout stream{
      .stride = sizeof(DebugLineVertex),
      .attributes = {
          {0, Format::kRGB32Float, offsetof(DebugLineVertex, pos)},
          {1, Format::kRGBA8Unorm, offsetof(DebugLineVertex, rgba)}}};
  GraphicsPipelineDesc desc{
      .vertex = RX_SHADER(k_debug_line_vs_hlsl),
      .fragment = RX_SHADER(k_debug_line_ps_hlsl),
      .vertex_buffers = {stream},
      .topology = PrimitiveTopology::kLineList,
      .raster = {.cull = CullMode::kNone,
                 .front = FrontFace::kCounterClockwise,
                 .polygon = PolygonMode::kFill},
      .depth = {.test = true,
                .write = false,
                .compare = CompareOp::kGreaterEqual,
                .format = kDepthFormat},
      .color_formats = {kSceneColorFormat},
      .blend = {BlendMode::kAlpha},
      .push_constant_size = sizeof(DebugLinePush),
      .debug_name = "debug_line",
  };
  debug_line_pipeline_ = device_->CreateGraphicsPipeline(desc);
  // Overlay variant: identical but no depth test, so it always draws on top.
  desc.depth = {.test = false, .write = false, .format = kDepthFormat};
  desc.debug_name = "debug_line_overlay";
  debug_line_overlay_pipeline_ = device_->CreateGraphicsPipeline(desc);
}

namespace {

// Built-in 4-wide, 6-tall uppercase stroke font for WorldText. Each glyph is a
// run of segments on that grid, emitted as camera-facing DebugLines (origin +
// right*x + up*y). Unlisted characters draw nothing.
void AppendGlyphBillboard(char c, f32 ox, f32 oy, f32 scale, const Vec3 &origin,
                          const Vec3 &right, const Vec3 &up, u32 rgba,
                          std::vector<DebugLine> &out) {
  auto seg = [&](f32 x0, f32 y0, f32 x1, f32 y1) {
    out.push_back({origin + right * ((ox + x0) * scale) + up * ((oy + y0) * scale),
                   origin + right * ((ox + x1) * scale) + up * ((oy + y1) * scale), rgba});
  };
  switch (c) {
    case 'A': seg(0,0,2,6); seg(2,6,4,0); seg(1,2,3,2); break;
    case 'B': seg(0,0,0,6); seg(0,6,3,6); seg(3,6,3,3); seg(3,3,0,3); seg(3,3,3,0); seg(3,0,0,0); break;
    case 'C': seg(4,6,0,6); seg(0,6,0,0); seg(0,0,4,0); break;
    case 'D': seg(0,0,0,6); seg(0,6,2,6); seg(2,6,4,4); seg(4,4,4,2); seg(4,2,2,0); seg(2,0,0,0); break;
    case 'E': seg(4,6,0,6); seg(0,6,0,0); seg(0,0,4,0); seg(0,3,3,3); break;
    case 'F': seg(4,6,0,6); seg(0,6,0,0); seg(0,3,3,3); break;
    case 'G': seg(4,6,0,6); seg(0,6,0,0); seg(0,0,4,0); seg(4,0,4,3); seg(4,3,2,3); break;
    case 'H': seg(0,0,0,6); seg(4,0,4,6); seg(0,3,4,3); break;
    case 'I': seg(2,0,2,6); seg(1,6,3,6); seg(1,0,3,0); break;
    case 'J': seg(4,6,4,1); seg(4,1,3,0); seg(3,0,1,0); seg(1,0,0,1); break;
    case 'K': seg(0,0,0,6); seg(0,3,4,6); seg(0,3,4,0); break;
    case 'L': seg(0,6,0,0); seg(0,0,4,0); break;
    case 'M': seg(0,0,0,6); seg(0,6,2,3); seg(2,3,4,6); seg(4,6,4,0); break;
    case 'N': seg(0,0,0,6); seg(0,6,4,0); seg(4,0,4,6); break;
    case 'O': seg(0,0,0,6); seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); break;
    case 'P': seg(0,0,0,6); seg(0,6,3,6); seg(3,6,3,3); seg(3,3,0,3); break;
    case 'Q': seg(0,0,0,6); seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); seg(2,2,4,0); break;
    case 'R': seg(0,0,0,6); seg(0,6,3,6); seg(3,6,3,3); seg(3,3,0,3); seg(1,3,4,0); break;
    case 'S': seg(4,6,0,6); seg(0,6,0,3); seg(0,3,4,3); seg(4,3,4,0); seg(4,0,0,0); break;
    case 'T': seg(0,6,4,6); seg(2,6,2,0); break;
    case 'U': seg(0,6,0,0); seg(0,0,4,0); seg(4,0,4,6); break;
    case 'V': seg(0,6,2,0); seg(2,0,4,6); break;
    case 'W': seg(0,6,1,0); seg(1,0,2,3); seg(2,3,3,0); seg(3,0,4,6); break;
    case 'X': seg(0,0,4,6); seg(0,6,4,0); break;
    case 'Y': seg(0,6,2,3); seg(4,6,2,3); seg(2,3,2,0); break;
    case 'Z': seg(0,6,4,6); seg(4,6,0,0); seg(0,0,4,0); break;
    case '0': seg(0,0,0,6); seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); seg(0,0,4,6); break;
    case '1': seg(1,4,2,6); seg(2,6,2,0); seg(1,0,3,0); break;
    case '2': seg(0,6,4,6); seg(4,6,4,3); seg(4,3,0,3); seg(0,3,0,0); seg(0,0,4,0); break;
    case '3': seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); seg(1,3,4,3); break;
    case '4': seg(0,6,0,3); seg(0,3,4,3); seg(3,6,3,0); break;
    case '5': seg(4,6,0,6); seg(0,6,0,3); seg(0,3,4,3); seg(4,3,4,0); seg(4,0,0,0); break;
    case '6': seg(4,6,0,6); seg(0,6,0,0); seg(0,0,4,0); seg(4,0,4,3); seg(4,3,0,3); break;
    case '7': seg(0,6,4,6); seg(4,6,1,0); break;
    case '8': seg(0,0,0,6); seg(0,6,4,6); seg(4,6,4,0); seg(4,0,0,0); seg(0,3,4,3); break;
    case '9': seg(4,0,4,6); seg(4,6,0,6); seg(0,6,0,3); seg(0,3,4,3); break;
    case '+': seg(2,1,2,5); seg(0,3,4,3); break;
    case '-': seg(0,3,4,3); break;
    case '.': seg(1,0,2,0); break;
    case '/': seg(0,0,4,6); break;
    case ':': seg(2,1,2,2); seg(2,4,2,5); break;
    default: break;
  }
}

void TessellateWorldText(const WorldText &t, const Vec3 &right, const Vec3 &up,
                         std::vector<DebugLine> &out) {
  const f32 scale = t.size / 6.0f;
  const f32 advance = 5.0f;  // grid units per glyph cell
  const f32 line_h = 8.0f;   // grid units per line
  size_t line_index = 0;
  size_t start = 0;
  const std::string &s = t.text;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i != s.size() && s[i] != '\n')
      continue;
    const size_t len = i - start;
    const f32 line_width = advance * static_cast<f32>(len);
    const f32 ox0 = -t.align * line_width;
    const f32 oy = -static_cast<f32>(line_index) * line_h;
    for (size_t j = 0; j < len; ++j) {
      char c = s[start + j];
      if (c >= 'a' && c <= 'z')
        c = static_cast<char>(c - 32);
      AppendGlyphBillboard(c, ox0 + static_cast<f32>(j) * advance, oy, scale, t.position, right, up,
                           t.rgba, out);
    }
    ++line_index;
    start = i + 1;
  }
}

}  // namespace

void Renderer::DrawDebugLines(CommandList &cmd, const FrameView &view,
                              const Mat4 &view_proj, Extent2D extent) {
  // Camera-facing basis so WorldText billboards stay upright and readable.
  Vec3 text_right = Cross(Normalize(view.camera.target - view.camera.eye), Vec3{0, 1, 0});
  if (Length(text_right) < 1e-4f)
    text_right = Vec3{1, 0, 0};
  text_right = Normalize(text_right);
  const Vec3 text_up = Normalize(Cross(text_right, Normalize(view.camera.target - view.camera.eye)));
  std::vector<DebugLine> text_depth, text_overlay;
  for (const WorldText &t : view.world_texts)
    TessellateWorldText(t, text_right, text_up, t.overlay ? text_overlay : text_depth);

  const size_t total = view.debug_lines.size() + view.debug_lines_overlay.size() +
                       text_depth.size() + text_overlay.size();
  if (total == 0)
    return;
  BuildDebugLinePipelines();
  if (!debug_line_pipeline_ || !debug_line_overlay_pipeline_)
    return;

  const u32 slot = frame_index_ % kFramesInFlight;
  const u32 needed_vertices = static_cast<u32>(total * 2);
  if (debug_line_vbo_capacity_[slot] < needed_vertices) {
    // Grow to the requested count (host-visible; the slot's fence already fired
    // in BeginFrame, so overwriting is safe). Round up to reduce churn.
    u32 cap = 256;
    while (cap < needed_vertices)
      cap *= 2;
    device_->DestroyBufferDeferred(debug_line_vbo_[slot]);
    debug_line_vbo_[slot] =
        device_->CreateBuffer(static_cast<u64>(cap) * sizeof(DebugLineVertex),
                              kBufferUsageVertex, /*host_visible=*/true);
    debug_line_vbo_capacity_[slot] = cap;
  }
  auto *verts = static_cast<DebugLineVertex *>(debug_line_vbo_[slot].mapped);
  if (!verts)
    return;

  auto append = [&](const DebugLine &l, u32 &at) {
    auto write = [&](const Vec3 &p) {
      verts[at].pos[0] = p.x;
      verts[at].pos[1] = p.y;
      verts[at].pos[2] = p.z;
      verts[at].rgba[0] = static_cast<u8>((l.rgba >> 24) & 0xff);
      verts[at].rgba[1] = static_cast<u8>((l.rgba >> 16) & 0xff);
      verts[at].rgba[2] = static_cast<u8>((l.rgba >> 8) & 0xff);
      verts[at].rgba[3] = static_cast<u8>(l.rgba & 0xff);
      ++at;
    };
    write(l.a);
    write(l.b);
  };

  u32 count = 0;
  const u32 depth_first = count;
  for (const DebugLine &l : view.debug_lines)
    append(l, count);
  for (const DebugLine &l : text_depth)
    append(l, count);
  const u32 depth_count = count - depth_first;
  const u32 overlay_first = count;
  for (const DebugLine &l : view.debug_lines_overlay)
    append(l, count);
  for (const DebugLine &l : text_overlay)
    append(l, count);
  const u32 overlay_count = count - overlay_first;

  DebugLinePush push{view_proj};
  cmd.SetViewport(0, 0, static_cast<f32>(extent.width),
                  static_cast<f32>(extent.height));
  cmd.SetScissor(0, 0, extent.width, extent.height);
  cmd.BindVertexBuffer(0, debug_line_vbo_[slot], 0);
  if (depth_count) {
    cmd.BindPipeline(debug_line_pipeline_);
    cmd.PushConstants(&push, sizeof(push), 0);
    cmd.Draw(depth_count, 1, depth_first, 0);
  }
  if (overlay_count) {
    cmd.BindPipeline(debug_line_overlay_pipeline_);
    cmd.PushConstants(&push, sizeof(push), 0);
    cmd.Draw(overlay_count, 1, overlay_first, 0);
  }
}

// --- Editor picking ----------------------------------------------------------

void Renderer::RequestPick(u32 x, u32 y) {
  pick_requested_ = true;
  pick_x_ = x;
  pick_y_ = y;
}

std::optional<PickResult> Renderer::TakePickResult() {
  if (!pick_result_ready_)
    return std::nullopt;
  pick_result_ready_ = false;
  return PickResult{pick_result_id_};
}

namespace {
struct PickPush {
  Mat4 mvp;
  u32 id;
};
} // namespace

void Renderer::RenderPickPass(const FrameView &view) {
  pick_requested_ = false;
  if (!device_ || device_->is_stub() || render_width_ == 0 ||
      render_height_ == 0)
    return;

  // (Re)create the id + depth targets at render resolution.
  if (pick_image_w_ != render_width_ || pick_image_h_ != render_height_ ||
      !pick_id_image_) {
    if (pick_id_image_)
      device_->DestroyImageDeferred(pick_id_image_);
    if (pick_depth_image_)
      device_->DestroyImageDeferred(pick_depth_image_);
    pick_id_image_ = device_->CreateImage2D(
        Format::kR32Uint, {render_width_, render_height_},
        kTextureUsageColorTarget | kTextureUsageTransferSrc);
    pick_depth_image_ = device_->CreateImage2D(Format::kD32Float,
                                               {render_width_, render_height_},
                                               kTextureUsageDepthTarget);
    pick_image_w_ = render_width_;
    pick_image_h_ = render_height_;
  }
  if (!pick_id_image_ || !pick_depth_image_)
    return;

  if (!pick_pipeline_) {
    pick_pipeline_ = device_->CreateGraphicsPipeline({
        .vertex = RX_SHADER(k_pick_id_vs_hlsl),
        .fragment = RX_SHADER(k_pick_id_ps_hlsl),
        .vertex_buffers = {{.stride = sizeof(asset::Vertex),
                            .attributes = {{0, Format::kRGB32Float,
                                            offsetof(asset::Vertex,
                                                     position)}}}},
        .raster = {.cull = CullMode::kBack,
                   .front = FrontFace::kCounterClockwise},
        .depth = {.test = true,
                  .write = true,
                  .compare = CompareOp::kGreaterEqual,
                  .format = Format::kD32Float},
        .color_formats = {Format::kR32Uint},
        .push_constant_size = sizeof(PickPush),
        .debug_name = "pick_id",
    });
  }
  if (!pick_pipeline_)
    return;

  const f32 aspect =
      static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
  const Mat4 view_proj = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f) *
                         LookAt(view.camera.eye, view.camera.target, {0, 1, 0});

  device_->ImmediateSubmit([&](CommandList &cmd) {
    cmd.Barrier(Transition(pick_id_image_, ResourceState::kUndefined,
                           ResourceState::kColorTarget));
    cmd.Barrier(Transition(pick_depth_image_, ResourceState::kUndefined,
                           ResourceState::kDepthTarget));
    ColorAttachment color{.view = pick_id_image_.view, .load = LoadOp::kClear};
    DepthAttachment depth{
        .view = pick_depth_image_.view, .load = LoadOp::kClear, .clear = 0.0f};
    cmd.BeginRendering({.extent = {render_width_, render_height_},
                        .colors = {&color, 1},
                        .depth = &depth});
    cmd.SetViewport(0, 0, static_cast<f32>(render_width_),
                    static_cast<f32>(render_height_));
    cmd.SetScissor(0, 0, render_width_, render_height_);
    cmd.BindPipeline(pick_pipeline_);
    for (const DrawItem &item : view.draws) {
      if (item.pick_id == 0)
        continue; // unpickable: leave the cleared 0
      const GpuMesh *mesh = meshes_.find(item.mesh);
      if (!mesh || !mesh->indices)
        continue;
      PickPush push{view_proj * item.transform, item.pick_id};
      cmd.PushConstants(&push, sizeof(push), 0);
      cmd.BindVertexBuffer(0, mesh->vertices, 0);
      cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
      for (const GpuSubmesh &submesh : mesh->submeshes) {
        cmd.DrawIndexed(submesh.index_count, 1, submesh.index_offset, 0, 0);
      }
    }
    cmd.EndRendering();
  });

  // Read back the whole id target and sample the requested pixel (in output
  // pixels, scaled to render resolution).
  std::vector<u32> pixels(static_cast<size_t>(render_width_) * render_height_);
  if (!device_->ReadbackImage(pick_id_image_, ResourceState::kColorTarget,
                              pixels.data(), pixels.size() * sizeof(u32))) {
    return;
  }
  u32 px = pick_x_, py = pick_y_;
  if (output_width_ > 0 && output_height_ > 0) {
    px = pick_x_ * render_width_ / output_width_;
    py = pick_y_ * render_height_ / output_height_;
  }
  if (px >= render_width_)
    px = render_width_ - 1;
  if (py >= render_height_)
    py = render_height_ - 1;
  pick_result_id_ = pixels[static_cast<size_t>(py) * render_width_ + px];
  pick_result_ready_ = true;
}

void Renderer::RecordDepthOnlyScene(CommandList &cmd,
                                    const Mat4 &light_view_proj,
                                    const FrameResources &frame,
                                    const FrameView &view) {
  BindingSetHandle bound_material{};
  // All the shadow caster pipelines share one layout, so pushes and set binds
  // persist across the per-submesh variant switches below. Bind the masked
  // static permutation up front so the matrix push always has a pipeline.
  PipelineHandle bound_pipeline = shadow_.pipeline();
  cmd.BindPipeline(bound_pipeline);
  cmd.PushConstants(&light_view_proj, sizeof(Mat4));
  for (const DrawItem &item : view.draws) {
    const GpuMesh *mesh = meshes_.find(item.mesh);
    // no_rt skips grass-like fill geometry, but skinned actors are
    // no_rt only to stay out of the tlas; they still cast shadows.
    // dynamic_vertices meshes always cast, even if a game marks them no_rt.
    if (!mesh || mesh->all_blend ||
        (mesh->no_rt && !mesh->skinned && !mesh->dynamic_vertices))
      continue;
    // Skinned casters run the bone-blended vertex stage so the
    // shadow tracks the animated pose, not the bind pose.
    bool draw_skinned = mesh->skinned && item.skin_offset >= 0 &&
                        static_cast<bool>(shadow_.skinned_pipeline());
    // The light matrix sits at offset 0; the model follows it, skin data after.
    cmd.PushConstants(&item.transform, sizeof(Mat4), sizeof(Mat4));
    cmd.BindVertexBuffer(0, mesh->vertices);
    if (draw_skinned) {
      cmd.BindVertexBuffer(1, mesh->skinning);
      struct {
        u64 bone_address;
        u32 skin_offset;
        u32 pad;
      } skin{frame.bone_palette.address, static_cast<u32>(item.skin_offset), 0};
      cmd.PushConstants(&skin, sizeof(skin), 2 * sizeof(Mat4));
    }
    cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
    for (const GpuSubmesh &submesh : mesh->submeshes) {
      if (submesh.blend)
        continue;
      // Opaque casters draw depth-only (no fragment, early-Z stays);
      // masked ones bind the alpha-test fragment + its material set.
      PipelineHandle pipeline =
          draw_skinned ? shadow_.skinned_pipeline(submesh.alpha_mask)
                       : shadow_.pipeline(submesh.alpha_mask);
      if (!(pipeline == bound_pipeline)) {
        cmd.BindPipeline(pipeline);
        bound_pipeline = pipeline;
      }
      if (submesh.alpha_mask) {
        BindingSetHandle material = material_system_->set(submesh.material);
        if (!(material == bound_material)) {
          cmd.BindSet(0, material);
          bound_material = material;
        }
      }
      cmd.DrawIndexed(submesh.index_count, 1, submesh.index_offset, 0, 0);
    }
  }
  f32 shadow_planes[5][4];
  ExtractFrustumPlanes(light_view_proj, shadow_planes);
  for (const InstanceStore::Group &group : instances_.groups()) {
    if (!group.alive)
      continue;
    if (group.cullable &&
        SphereOutsideFrustum(shadow_planes, group.bounds_center,
                             group.bounds_radius))
      continue;
    const GpuMesh *mesh = meshes_.find(group.mesh);
    if (!mesh || mesh->all_blend || (mesh->no_rt && !mesh->dynamic_vertices))
      continue;
    cmd.BindVertexBuffer(0, mesh->vertices);
    cmd.BindVertexBuffer(1, group.buffer);
    cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
    for (const GpuSubmesh &submesh : mesh->submeshes) {
      if (submesh.blend)
        continue;
      const PipelineHandle pipeline =
          shadow_.instanced_pipeline(submesh.alpha_mask);
      if (!(pipeline == bound_pipeline)) {
        cmd.BindPipeline(pipeline);
        bound_pipeline = pipeline;
      }
      if (submesh.alpha_mask) {
        const BindingSetHandle material =
            material_system_->set(submesh.material);
        if (!(material == bound_material)) {
          cmd.BindSet(0, material);
          bound_material = material;
        }
      }
      cmd.DrawIndexed(submesh.index_count,
                      static_cast<u32>(group.transforms.size()),
                      submesh.index_offset, 0, 0);
    }
  }
}

void Renderer::BuildFrameGraph(FrameResources &frame, u32 image_index,
                               const FrameView &view) {
  u32 frame_slot = frame_index_ % kFramesInFlight;
  bool rt_shadows = rt_available_ && settings_.rt_shadows;
  bool rtao_active = rt_available_ && settings_.rtao;
  // RCGI takes over the indirect-diffuse path (DDGI + SSGI) when on. It is
  // available with hardware ray query OR the software SDF clipmap tracer.
  bool sdf_ready = sdf_clipmap_ && sdf_clipmap_->ready() && sdf_available_;
  bool rcgi_active = rcgi_ && settings_.rcgi && settings_.ibl &&
                     bindless_ != nullptr && (rt_available_ || sdf_ready);
  // Software mode: forced by a non-ray-query device or RX_RCGI_SW (A/B on RT
  // hardware). Needs the SDF clipmap; the world side then traces the clipmap
  // and the resolve is forced to the probes-only path (the M2 gather is
  // ray-query).
  bool rcgi_software =
      rcgi_active && sdf_ready && (!rt_available_ || rcgi_force_software_);
  bool rcgi_probes_only = RcgiProbesOnlyOpt || rcgi_software;
  bool ddgi_active = ddgi_ && settings_.ddgi && settings_.ibl && !rcgi_active;
  bool reflections_active =
      rt_available_ && settings_.rt_reflections && bindless_ != nullptr;
  // The ray-query fragment variant serves both shadows and reflections.
  bool use_rt_frag = rt_shadows || reflections_active;
  bool path_trace =
      rt_available_ && bindless_ != nullptr && settings_.path_trace;
  bool rcgi_world = rcgi_active && !path_trace;
  if (rcgi_ && !rcgi_world)
    rcgi_->RequestReset();
  // Set by the raster path when the 3D precipitation volume draws; the
  // post-resolve screen-space streak fallback is skipped that frame.
  bool precip_volume_drawn = false;
  // kMsaa: the prepass + opaque scene render multisampled and resolve before
  // everything downstream, which then runs single-sampled exactly as kNone.
  // ApplySettings already rebuilt the mesh pipelines at this sample count.
  const bool msaa = applied_msaa_samples_ > 1 && !path_trace;
  const u32 msaa_samples = msaa ? applied_msaa_samples_ : 1;
  // Scene pass consumes last frame's rate image; the rebuild pass below the
  // transparents keeps it fresh. Wireframe wants exact per-pixel lines.
  // The VRS rate image cannot attach to a multisampled pass here.
  vrs_active_ = settings_.vrs && vrs_.available() && !path_trace &&
                !settings_.wireframe && !msaa;
  // Foliage uploaded before path tracing was enabled has no blas (it was
  // excluded from the realtime tlas). Build it now so alpha-tested vegetation
  // appears when path tracing is toggled on at runtime, not only when set
  // before content load.
  if (rt_geometry_dirty_ || (path_trace && rt_foliage_dirty_)) {
    const bool ready = EnsureRayTracingGeometry();
    rt_geometry_dirty_ = !ready;
    if (path_trace && ready)
      rt_foliage_dirty_ = false;
  }
  bool fog_active = rt_available_ && settings_.fog && !path_trace;
  // Ambient occlusion technique: ray-traced + NRD-denoised when available, else
  // the screen-space fallback so non-rt tiers (and forced low presets) keep ao.
  bool nrd_ao = false;
  bool nrd_shadow = false;
#if defined(RX_HAS_NRD)
  nrd_ao = rtao_active && nrd_.available();
  nrd_shadow = rt_shadows && nrd_.available();
#endif
  // Denoised stochastic reflections need the NRD specular denoiser; without
  // it the rt fragment variant keeps its inline deterministic mirror ray.
  bool spec_refl_active = false;
#if defined(RX_HAS_NRD)
  spec_refl_active = reflections_active && nrd_.available() && !path_trace;
#endif
  bool ss_ao = settings_.ssao && !nrd_ao && !path_trace;
  // Cascaded shadow maps: the raster sun-shadow path, used whenever ray-traced
  // shadows are not. The rt fragment variant traces its own shadow ray instead.
  bool csm_active = settings_.shadow_maps && !rt_shadows && !path_trace;
  // Screen-space reflections stand in for ray-traced reflections on raster
  // tiers.
  bool ssr_active = settings_.ssr && !path_trace && !reflections_active;
  // Screen-space gi stands in for the ddgi probe volume on raster tiers.
  bool ssgi_active =
      settings_.ssgi && !path_trace && !ddgi_active && !rcgi_active;
  time_seconds_ += view.frame_delta_seconds;

  // Transparent work is gathered up front: water forces a tlas (the water
  // pipeline statically binds it) and an opaque snapshot pass.
  struct TransparentDraw {
    const DrawItem *item;
    const GpuSubmesh *submesh;
    f32 distance_sq;
  };
  base::Vector<TransparentDraw> transparent;
  transparent.reserve(view.draws.size());
  bool any_water = false;
  const DrawItem *adaptive_water_item = nullptr;
  const GpuSubmesh *adaptive_water_submesh = nullptr;
  f32 adaptive_water_area = 0.0f;
  for (const DrawItem &item : view.draws) {
    const GpuMesh *mesh = meshes_.find(item.mesh);
    if (!mesh)
      continue;
    for (const GpuSubmesh &submesh : mesh->submeshes) {
      if (!submesh.blend)
        continue;
      f32 dx = item.transform.m[12] - view.camera.eye.x;
      f32 dy = item.transform.m[13] - view.camera.eye.y;
      f32 dz = item.transform.m[14] - view.camera.eye.z;
      transparent.push_back({&item, &submesh, dx * dx + dy * dy + dz * dz});
      if (submesh.water) {
        any_water = true;
        if (settings_.adaptive_water && water_ &&
            water_->adaptive_available() && mesh->planar_water) {
          f32 area = (mesh->water_bounds[2] - mesh->water_bounds[0]) *
                     (mesh->water_bounds[3] - mesh->water_bounds[1]);
          if (area > adaptive_water_area) {
            adaptive_water_area = area;
            adaptive_water_item = &item;
            adaptive_water_submesh = &submesh;
          }
        }
      }
    }
  }
  bool water_pipeline_active = any_water && water_ != nullptr;
  // Volumetric precipitation's optional per-particle sun rays consult the TLAS
  // from its vertex shader, so an active rainstorm must keep the TLAS built
  // and current even when every other ray-traced effect is disabled.
  const bool precip_volume_ready =
      settings_.weather.volumetric && settings_.weather.precipitation > 0.0f &&
      !settings_.interior && precip_volume_.available();
  const bool precip_rt = precip_volume_ready && settings_.weather.rt_shadows &&
                         rt_available_ && !path_trace;
  // When the tlas is consulted for shading, the rasterized surface must match
  // the blas (built at lod 0), or rays can hit geometry that the selected
  // raster lod did not draw. This includes water reflections and volumetric-fog
  // visibility, not only the opaque scene's ray-traced effects.
  bool force_lod0_for_tlas =
      rt_shadows || rtao_active || ddgi_active ||
      (rcgi_active && !rcgi_software) || reflections_active || path_trace ||
      (water_pipeline_active && settings_.water_reflections) || fog_active ||
      precip_rt;

  // Async TLAS build (RX_RT_ASYNC_TLAS): build this frame's slot on the compute
  // queue while the graphics timeline consumes the slot built last frame -- the
  // slot being built is never the slot being read this frame, so there is no
  // same-frame cross-queue hazard on the acceleration structure at all; the
  // async submit is waited by SubmitFrame and a full frame elapses before the
  // slot is read, which the frame fence already guarantees. This rides the same
  // async-fork discipline the DDGI/RCGI world passes already use to read the
  // TLAS across queues (validated clean). Needs a second queue and one prior
  // frame to have primed a slot; the path tracer keeps the synchronous same-
  // slot build for reference correctness. Three ping-pong slots keep a slot
  // safe to rebuild while one frame still reads the previous one at two frames
  // in flight (see RayTracingContext::kSlots).
  // The async read slot is the previous frame's build. It is only safe when
  // that slot actually holds a current build: RT enabled after raster-only
  // frames leaves it unbuilt, and a mesh replace (WaitIdle + RemoveBlas)
  // retires every slot whose instances still point at the freed BLAS.
  // TlasSlotTracker tracks both and falls the selection back to a synchronous
  // build+read of the current slot when the previous one is invalid.
  bool want_async_tlas = RtAsyncTlasOpt && device_->caps().async_compute &&
                         settings_.async_compute && !path_trace &&
                         frame_index_ > 0;
  TlasSlotTracker::Selection tlas_sel =
      raytracing_ ? raytracing_->SelectTlasSlots(frame_index_, want_async_tlas)
                  : TlasSlotTracker{}.Select(frame_index_, false);
  bool async_tlas = tlas_sel.async;
  u32 tlas_build_slot = tlas_sel.build_slot;
  u32 tlas_slot = tlas_sel.read_slot;

  // The frame's globals set (uniform + optional tlas + optional hi-z) is
  // rewritten once per frame, from the first pass that needs it. The slot's
  // fence has fired, so its previous frame no longer reads the set.
  BindingSetHandle globals_set = globals_sets_[frame_slot];
  auto update_globals_set =
      [this, globals_set, tlas_slot](PassContext &ctx, ResourceHandle cull_hiz,
                                     bool ms_active, bool want_tlas) {
        base::Vector<BindingItem> items;
        items.push_back(
            Bind::Uniform(0, frames_[frame_index_ % kFramesInFlight].globals, 0,
                          sizeof(FrameGlobals)));
        if (want_tlas && rt_available_ && raytracing_ &&
            raytracing_->tlas(tlas_slot)) {
          items.push_back(Bind::Accel(1, raytracing_->tlas(tlas_slot)));
        }
        if (ms_active) { // hi-z for the task-stage occlusion cull (real or
                         // fallback)
          TextureView hiz = cull_hiz != kInvalidResource
                                ? ctx.graph->image(cull_hiz).view
                                : ms_dummy_hiz_.view;
          items.push_back(Bind::SampledView(2, hiz));
        }
        device_->UpdateBindingSet(globals_set, {items.data(), items.size()});
      };

  // Water + transparency over an opaque base. A lambda (rather than inline) so
  // the path tracer, which otherwise skips the whole raster transparency path,
  // can composite water over its result too. Consumes `transparent` (moved into
  // the pass), so it runs at most once per frame. Returns the composited
  // colour.
  auto add_water =
      [&](ResourceHandle scene_color, ResourceHandle depth,
          ResourceHandle depth_export, ResourceHandle motion,
          ResourceHandle sun_shadow, ResourceHandle shadow_atlas, bool csm_on,
          u32 shadow_slot, u32 water_tlas_slot, bool globals_written,
          ResourceHandle rcgi_irr = kInvalidResource) -> ResourceHandle {
    std::sort(transparent.begin(), transparent.end(),
              [](const TransparentDraw &a, const TransparentDraw &b) {
                return a.distance_sq > b.distance_sq;
              });

    // Transparency renders into a copy of the opaque result and refracts by
    // sampling the original, which never returns to attachment layout
    // afterwards: re-attaching a sampled image corrupts its compression
    // metadata on nvidia (the depth export exists for the same reason).
    ResourceHandle composite =
        graph_.CreateTexture({.name = "composite",
                              .format = kSceneColorFormat,
                              .width = render_width_,
                              .height = render_height_});
    graph_.AddPass(
        "opaque_copy",
        [&](RenderGraph::PassBuilder &builder) {
          builder.Read(scene_color, ResourceUsage::kSampledCompute);
          builder.Write(composite, ResourceUsage::kStorageWrite);
        },
        [this, scene_color, composite](PassContext &ctx) {
          water_->RecordCopy(ctx, scene_color, composite, render_width_,
                             render_height_);
        });

    ResourceHandle opaque_color = scene_color;
    ResourceHandle opaque_depth = depth_export;
    graph_.AddPass(
        "transparent",
        [&](RenderGraph::PassBuilder &builder) {
          builder.Write(composite, ResourceUsage::kColorAttachment);
          builder.Write(motion, ResourceUsage::kColorAttachment);
          builder.Write(depth, ResourceUsage::kDepthAttachment);
          builder.Read(opaque_color, ResourceUsage::kSampledFragment);
          builder.Read(opaque_depth, ResourceUsage::kSampledFragment);
          if (sun_shadow != kInvalidResource)
            builder.Read(sun_shadow, ResourceUsage::kSampledFragment);
          if (csm_on)
            builder.Read(shadow_atlas, ResourceUsage::kSampledFragment);
          if (rcgi_irr != kInvalidResource)
            builder.Read(rcgi_irr, ResourceUsage::kSampledFragment);
        },
        [this, composite, motion, depth, opaque_color, opaque_depth, sun_shadow,
         water_tlas_slot, use_rt_frag, ddgi_active, water_pipeline_active,
         csm_on, shadow_slot, shadow_atlas, globals_set, globals_written,
         update_globals_set, frame_slot, adaptive_water_item,
         adaptive_water_submesh, transparent = std::move(transparent), rcgi_irr,
         rcgi_world, &frame, &view](PassContext &ctx) {
          if (!globals_written) {
            update_globals_set(ctx, kInvalidResource, false,
                               /*want_tlas=*/true);
          }

          BindingSetHandle env_set = env_transparent_sets_[frame_slot];
          EnvironmentSystem::DdgiBinding ddgi_binding;
          if (ddgi_active)
            ddgi_binding = ddgi_->binding(frame_index_);
          TextureView sun_shadow_view = sun_shadow != kInvalidResource
                                            ? ctx.graph->image(sun_shadow).view
                                            : TextureView{};
          TextureView rcgi_irr_view = rcgi_irr != kInvalidResource
                                          ? ctx.graph->image(rcgi_irr).view
                                          : TextureView{};
          EnvironmentSystem::RcgiWorldBinding rcgi_world_binding;
          RcgiSystem::IrradianceBinding rcgi_world_src;
          if (rcgi_world)
            rcgi_world_src = rcgi_->irradiance_binding(frame_index_);
          if (rcgi_world_src.valid) {
            rcgi_world_binding.irradiance = rcgi_world_src.irradiance;
            rcgi_world_binding.visibility = rcgi_world_src.visibility;
            rcgi_world_binding.globals = rcgi_world_src.globals;
            rcgi_world_binding.probe_meta = rcgi_world_src.probe_meta;
            rcgi_world_binding.interior_vols = rcgi_world_src.interior_vols;
          }
          environment_->WriteEnvSet(
              env_set, TextureView{}, ddgi_active ? &ddgi_binding : nullptr,
              csm_on ? ctx.graph->image(shadow_atlas).view : TextureView{},
              csm_on ? shadow_.cascade_buffer(shadow_slot) : GpuBuffer{},
              shadow_.cascade_buffer_size(),
              ctx.graph->image(opaque_color).view, sun_shadow_view,
              frame.lights, frame.lights.size, TextureView{}, cluster_counts_,
              cluster_indices_, frame.decals, decal_cluster_indices_,
              decal_atlas_view_,
              local_shadows_active_ ? local_shadows_.face_buffer(frame_slot)
                                    : GpuBuffer{},
              local_shadows_active_ ? local_shadows_.atlas().view
                                    : TextureView{},
              decal_normal_atlas_view_, TextureView{}, TextureView{},
              GpuBuffer{}, TextureView{}, TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : TextureView{},
              water_field_active_ ? water_field_.ring_view(0) : TextureView{},
              water_field_active_ ? water_field_.ring_view(1) : TextureView{},
              water_field_active_ ? water_field_.params_buffer(frame_slot)
                                  : GpuBuffer{},
              TextureView{}, TextureView{}, rcgi_irr_view,
              rcgi_world_src.valid ? &rcgi_world_binding : nullptr);

          // Update the dominant planar surface before beginning rasterization.
          // Its CBT/vertex/indirect buffers persist inside WaterPass; this
          // dispatch only changes leaf slots whose LOD decision changed.
          if (adaptive_water_item && adaptive_water_submesh) {
            if (const GpuMesh *adaptive_mesh =
                    meshes_.find(adaptive_water_item->mesh)) {
              const f32 aspect = static_cast<f32>(render_width_) /
                                 static_cast<f32>(std::max(render_height_, 1u));
              Mat4 vp = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f) *
                        LookAt(view.camera.eye, view.camera.target, {0, 1, 0});
              Vec3 camera_local = TransformPoint(
                  Inverse(adaptive_water_item->transform), view.camera.eye);
              AdaptiveWaterMesh::UpdateParams params;
              params.local_to_clip = vp * adaptive_water_item->transform;
              std::copy_n(adaptive_mesh->water_bounds, 4, params.bounds);
              params.camera_local = camera_local;
              params.height = adaptive_mesh->water_height;
              params.time = static_cast<f32>(time_seconds_);
              params.target_pixels = settings_.water_target_triangle_pixels;
              params.render_width = render_width_;
              params.render_height = render_height_;
              params.triangle_budget = settings_.water_triangle_budget;
              params.surface_key = adaptive_water_item->mesh;
              water_->UpdateAdaptive(*ctx.cmd, params);
            }
          }

          ColorAttachment colors[2];
          colors[0] = {.view = ctx.graph->image(composite).view,
                       .load = LoadOp::kLoad};
          colors[1] = {.view = ctx.graph->image(motion).view,
                       .load = LoadOp::kLoad};
          DepthAttachment depth_attachment{.view = ctx.graph->image(depth).view,
                                           .load = LoadOp::kLoad};
          ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                   .colors = {colors, 2},
                                   .depth = &depth_attachment});

          // Effect-shader fire/glows use the additive blend pipeline; the same
          // unlit fragment shader serves both the alpha and additive effect
          // materials (it premultiplies coverage for the additive one).
          enum class Mode { kNone, kWater, kBlend, kBlendAdditive };
          Mode mode = Mode::kNone;
          BindingSetHandle bound_material{};
          const DrawItem *bound_item = nullptr;
          for (const TransparentDraw &draw : transparent) {
            const GpuMesh *mesh = meshes_.find(draw.item->mesh);
            if (!mesh)
              continue;
            bool as_water = draw.submesh->water && water_pipeline_active;
            bool adaptive_draw = as_water && draw.item == adaptive_water_item &&
                                 draw.submesh == adaptive_water_submesh;
            bool additive = draw.submesh->effect_additive;

            Mode wanted =
                as_water ? Mode::kWater
                         : (additive ? Mode::kBlendAdditive : Mode::kBlend);
            if (mode != wanted) {
              BindingSetHandle bindless_set =
                  bindless_ ? bindless_->set() : BindingSetHandle{};
              if (as_water) {
                water_->Bind(ctx, globals_set, env_set, bindless_->set(),
                             opaque_color, opaque_depth);
              } else if (additive) {
                mesh_pipeline_->BindBlendAdditive(
                    *ctx.cmd, globals_set, env_set, bindless_set, use_rt_frag);
              } else {
                mesh_pipeline_->BindBlend(*ctx.cmd, globals_set, env_set,
                                          bindless_set, use_rt_frag);
              }
              mode = wanted;
              bound_material = {};
              bound_item = nullptr;
            }
            if (draw.item != bound_item) {
              MeshPushConstants push{.model = draw.item->transform,
                                     .prev_model = draw.item->prev_transform};
              // The blend pipelines run the static vertex path, which still
              // applies morphs (only skinning needs the extra vertex stream).
              if (mesh->morph_target_count > 0 &&
                  draw.item->morph_offset >= 0 && draw.item->morph_count > 0) {
                push.morph_delta_address = mesh->morph_deltas.address;
                push.morph_weight_address = frame.morph_weights.address;
                push.morph_first = static_cast<u32>(draw.item->morph_offset);
                push.morph_count = draw.item->morph_count;
                push.morph_vertex_count = mesh->vertex_count;
              }
              if (as_water) {
                // The water pipeline shares the mesh push block. Push the whole
                // struct so detail_rect is zeroed: mesh.vs sinks vertices
                // inside a nonzero rect, and a stale one would sink the water
                // plane.
                ctx.cmd->PushConstants(&push, sizeof(push));
                if (!adaptive_draw) {
                  ctx.cmd->BindVertexBuffer(0, mesh->vertices);
                  ctx.cmd->BindIndexBuffer(mesh->indices, 0,
                                           IndexType::kUint32);
                }
              } else {
                mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
              }
              bound_item = draw.item;
            }
            BindingSetHandle material =
                material_system_->set(draw.submesh->material);
            if (!(material == bound_material)) {
              if (as_water) {
                water_->BindMaterial(*ctx.cmd, material);
              } else {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
              }
              bound_material = material;
            }
            if (adaptive_draw) {
              water_->DrawAdaptive(*ctx.cmd);
              // A following authored submesh may share this DrawItem; force it
              // to restore the source vertex/index buffers.
              bound_item = nullptr;
            } else {
              mesh_pipeline_->DrawSubmesh(*ctx.cmd, *draw.submesh);
            }
          }

          // Heightfield fluid surface (flowing water + lava). Drawn last in the
          // transparent pass over the same color+motion+depth targets: it binds
          // its own pipeline and a transient set wrapping the solver's GENERAL
          // images, so the mesh/water bindings above do not need restoring. The
          // solver's final barrier (fluid_sim.cc) made the state graphics-
          // readable. Depth write + reversed-z Greater resolve the lava-under-
          // water ordering regardless of instance raster order.
          if (fluid_sim_active_ && fluid_sim_.active() && fluid_surface_) {
            fluid_surface_->Draw(ctx, globals_set, env_set, fluid_sim_,
                                 frame_slot, static_cast<f32>(time_seconds_));
          }
          ctx.cmd->EndRendering();
        });
    return composite;
  };

  // Camera state for both this frame and reprojection. Jitter lives in the
  // projection, not the matrices used for motion vectors.
  f32 aspect = static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
  // Orthographic main view (isometric / top-down / 2.5D) when the camera asks
  // for it; otherwise the reversed-z infinite-far perspective every other path
  // uses. The ortho matrix shares the reversed-z clip space so the depth-aware
  // passes below are unchanged.
  Mat4 proj;
  if (view.camera.ortho_height > 0.0f) {
    f32 half_h = view.camera.ortho_height * 0.5f;
    f32 half_w = half_h * aspect;
    proj = OrthographicReversedZ(-half_w, half_w, -half_h, half_h, view.camera.ortho_near,
                                 view.camera.ortho_far);
  } else {
    proj = PerspectiveReversedZ(view.camera.fov_y, aspect, 0.1f);
  }
  Mat4 view_mat = LookAt(view.camera.eye, view.camera.target, {0, 1, 0});
  Mat4 view_proj = proj * view_mat;

  // Streaming feedback: touch the materials of frustum-visible draws only.
  // view.draws is the full submitted list (GPU culling happens later), so
  // without the sphere test every loaded material would read as hot and the
  // texture LRU would degenerate to "loaded".
  if (material_system_ && material_system_->streaming_active()) {
    f32 touch_planes[5][4];
    ExtractFrustumPlanes(view_proj, touch_planes);
    for (const DrawItem &item : view.draws) {
      const GpuMesh *mesh = meshes_.find(item.mesh);
      if (!mesh)
        continue;
      const f32 *m = item.transform.m;
      const f32 *c = mesh->bounds_center;
      Vec3 wc{m[0] * c[0] + m[4] * c[1] + m[8] * c[2] + m[12],
              m[1] * c[0] + m[5] * c[1] + m[9] * c[2] + m[13],
              m[2] * c[0] + m[6] * c[1] + m[10] * c[2] + m[14]};
      f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
      f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
      f32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
      f32 radius = mesh->bounds_radius * std::max(sx, std::max(sy, sz));
      if (radius > 0.0f && SphereOutsideFrustum(touch_planes, wc, radius))
        continue;
      for (const GpuSubmesh &submesh : mesh->submeshes) {
        material_system_->Touch(submesh.material, frame_index_);
      }
    }
    for (const InstanceStore::Group &group : instances_.groups()) {
      if (!group.alive ||
          (group.cullable &&
           SphereOutsideFrustum(touch_planes, group.bounds_center,
                                group.bounds_radius))) {
        continue;
      }
      const GpuMesh *mesh = meshes_.find(group.mesh);
      if (!mesh)
        continue;
      for (const GpuSubmesh &submesh : mesh->submeshes) {
        material_system_->Touch(submesh.material, frame_index_);
      }
    }
  }

  bool temporal = settings_.aa_mode == AntiAliasingMode::kTaa ||
                  settings_.aa_mode == AntiAliasingMode::kUpscaler;
  f32 jitter_x = 0, jitter_y = 0;
  if (temporal) {
    u32 sample_count = taa_.settings().jitter_sample_count;
    if (settings_.aa_mode == AntiAliasingMode::kUpscaler) {
      // FSR-style phase count grows with the scale factor squared.
      f32 scale =
          static_cast<f32>(output_width_) / static_cast<f32>(render_width_);
      sample_count = static_cast<u32>(std::ceil(8.0f * scale * scale));
    }
    JitterSequence::Sample(frame_index_, sample_count, &jitter_x, &jitter_y);
  }

  bool first_frame = !has_prev_frame_;

  FrameGlobals globals;
  globals.view_proj = view_proj;
  globals.prev_view_proj = has_prev_frame_ ? prev_view_proj_ : view_proj;
  globals.inv_view_proj = Inverse(view_proj);
  globals.jitter[0] = 2.0f * jitter_x / static_cast<f32>(render_width_);
  globals.jitter[1] = 2.0f * jitter_y / static_cast<f32>(render_height_);
  // Interior cells author their own lighting (XCLL/LGTM): the directional fill
  // rides the sun path with the authored colour/direction, and the sky-derived
  // sun/atmosphere/IBL are suppressed below.
  const bool interior = settings_.interior;
  Vec3 sun = Normalize(interior ? settings_.interior_directional_dir
                                : settings_.sun_direction);
  globals.sun_direction[0] = sun.x;
  globals.sun_direction[1] = sun.y;
  globals.sun_direction[2] = sun.z;
  // Lightning flashes the PER-FRAME direct light only (not settings_, so the
  // sun-change check never rebuilds the IBL cubemap): a brief bright blue-white
  // boost to the directional intensity, colour and ambient fill.
  const f32 flash = interior ? 0.0f : settings_.weather.lightning;
  if (interior) {
    globals.sun_direction[3] = settings_.interior_directional_intensity;
    globals.sun_color[0] = settings_.interior_directional_color.x;
    globals.sun_color[1] = settings_.interior_directional_color.y;
    globals.sun_color[2] = settings_.interior_directional_color.z;
    globals.sun_color[3] = 0.0f;
    globals.interior_ambient[0] = settings_.interior_ambient.x;
    globals.interior_ambient[1] = settings_.interior_ambient.y;
    globals.interior_ambient[2] = settings_.interior_ambient.z;
    globals.interior_fog_color0[0] = settings_.interior_fog_near_color.x;
    globals.interior_fog_color0[1] = settings_.interior_fog_near_color.y;
    globals.interior_fog_color0[2] = settings_.interior_fog_near_color.z;
    globals.interior_fog_color0[3] = settings_.interior_fog_near;
    globals.interior_fog_color1[0] = settings_.interior_fog_far_color.x;
    globals.interior_fog_color1[1] = settings_.interior_fog_far_color.y;
    globals.interior_fog_color1[2] = settings_.interior_fog_far_color.z;
    globals.interior_fog_color1[3] = settings_.interior_fog_far;
    globals.interior_fog_params[0] = settings_.interior_fog_power;
    globals.interior_fog_params[1] = settings_.interior_fog_max;
  } else {
    globals.sun_direction[3] = settings_.sun_intensity + flash * 9.0f;
    globals.sun_color[0] =
        settings_.sun_color.x + flash * (0.90f - settings_.sun_color.x);
    globals.sun_color[1] =
        settings_.sun_color.y + flash * (0.95f - settings_.sun_color.y);
    globals.sun_color[2] =
        settings_.sun_color.z + flash * (1.10f - settings_.sun_color.z);
    globals.sun_color[3] = settings_.ambient + flash * 0.5f;
  }
  globals.camera_position[0] = view.camera.eye.x;
  globals.camera_position[1] = view.camera.eye.y;
  globals.camera_position[2] = view.camera.eye.z;
  globals.camera_position[3] = settings_.ibl_intensity;
  globals.misc[0] = static_cast<f32>(render_width_);
  globals.misc[1] = static_cast<f32>(render_height_);
  globals.misc[2] = settings_.sun_angular_radius;
  globals.misc[3] = static_cast<f32>(frame_index_ % 4096);
  if (settings_.ibl && !interior)
    globals.flags |= kFrameFlagIbl;
  if (nrd_ao || ss_ao)
    globals.flags |= kFrameFlagAoValid;
  if (csm_active && !interior)
    globals.flags |= kFrameFlagShadowMap;
  if (ddgi_active && !interior)
    globals.flags |= kFrameFlagDdgi;
  // RCGI composites in the IBL branch outdoors; indoors it now feeds the
  // interior branch instead (mesh.ps), lighting interiors with leak-free bounce
  // (its ray misses fall back to interior ambient) rather than a flat authored
  // term. Gated by RX_RCGI_INTERIOR so recreation's default (RCGI off) is
  // unchanged.
  if (rcgi_world && (!interior || RcgiInteriorOpt))
    globals.flags |= kFrameFlagRcgi;
  if (water_pipeline_active && settings_.water_reflections)
    globals.flags |= kFrameFlagWaterRt;
  if (rt_shadows && !interior)
    globals.flags |= kFrameFlagRtShadows;
  if (settings_.weather.aurora && !interior)
    globals.flags |= kFrameFlagAurora;
  // Aurora intensity rides the otherwise-unused pad_wind slot; sky.ps mirrors
  // it as `aurora.x` and night-gates it itself. Zero keeps the effect off.
  // pad_wind[1] carries the app's explicit night factor (moon-lit nights point
  // the "sun" downward, so the shader's elevation fallback reads as day there).
  globals.pad_wind[0] = (settings_.weather.aurora && !interior)
                            ? settings_.weather.aurora_intensity
                            : 0.0f;
  globals.pad_wind[1] = settings_.night;
  if (nrd_shadow && !interior)
    globals.flags |= kFrameFlagSigmaShadow;
  if (interior)
    globals.flags |= kFrameFlagInterior;
  if (reflections_active)
    globals.flags |= kFrameFlagReflections;
  if (spec_refl_active)
    globals.flags |= kFrameFlagSpecReflTex;
  globals.time = static_cast<f32>(time_seconds_);
  globals.debug_view = static_cast<u32>(settings_.debug_view);
  globals.reflection_cutoff = settings_.reflection_roughness_cutoff;
  globals.ao_ray_count =
      nrd_ao ? settings_.ao_rays : 0u; // rt ao rays, for the ray-count view

  // Dynamic point lights: copy into the host-visible frame buffer (capped).
  u32 light_count =
      std::min<u32>(static_cast<u32>(view.lights.size()), kMaxFrameLights);
  if (light_count > 0) {
    std::memcpy(frame.lights.mapped, view.lights.data(),
                light_count * sizeof(PointLight));
  }
  // Active lightning strike: append the positioned flash light after the copy
  // so it clusters, claims local-shadow faces and fills the froxel volumetrics
  // like any other light (and ReSTIR/clustered reflections see the flash even
  // though the bolt itself is a raster overlay). The GLOBAL weather.lightning
  // sun/ambient boost above is separate; this adds locality on top.
  if (!interior && !path_trace) {
    light_count += lightning_.AppendLights(
        static_cast<PointLight *>(frame.lights.mapped) + light_count,
        kMaxFrameLights - light_count, settings_.weather);
  }
  // Local light shadows: the nearest casters claim atlas faces (writes each
  // claimed light's params.w in the mapped buffer, so it must follow the copy).
  local_shadows_active_ = false;
  if (settings_.local_shadows && !path_trace && light_count > 0) {
    local_shadows_.Assign(static_cast<PointLight *>(frame.lights.mapped),
                          light_count, view.camera.eye, frame_slot);
    local_shadows_active_ = local_shadows_.face_count() > 0;
  }
  u32 decal_count =
      std::min<u32>(static_cast<u32>(view.decals.size()), kMaxFrameDecals);
  if (decal_count > 0) {
    std::memcpy(frame.decals.mapped, view.decals.data(),
                decal_count * sizeof(Decal));
  }
  globals.light_count = light_count;
  // The FFT ocean, interaction field, shoreline wetting and caustics all
  // describe a water surface, and every water surface (sea, CBT sheet, lake)
  // reaches the renderer as a water-material submesh, so gate the whole family
  // on one being submitted. These features default on, and "available()" only
  // means the pipelines exist -- without this gate every scene paid for the
  // sims, and worse, caustics modulated the sun on everything below the rest
  // height (y=0 by default): wavy grey mottling across dry ground.
  const bool scene_has_water = water_pipeline_active;
  fft_ocean_active_ = settings_.fft_ocean && ocean_.available() &&
                      !path_trace && !interior && scene_has_water;
  const bool fft_ocean_active = fft_ocean_active_;
  if (fft_ocean_active)
    globals.flags |= kFrameFlagFftOcean;
  water_field_active_ = settings_.water_field && water_field_.available() &&
                        !path_trace && !interior && scene_has_water;
  if (water_field_active_)
    globals.flags |= kFrameFlagWaterField;
  // The optional fluid solver runs whenever a domain is submitted; it is NOT
  // gated on scene_has_water (a lava-only scene carries no water material). It
  // IS gated on the surface draw being reachable: the fluid draws inside the
  // transparent pass, which needs water_ (only created with ray query) — on a
  // non-RT device the solver would otherwise burn GPU every frame with no
  // visual output.
  fluid_sim_active_ = settings_.fluid_sim && fluid_sim_.available() &&
                      water_ != nullptr && fluid_surface_ != nullptr &&
                      !path_trace && view.fluid_domain != nullptr;
  // Shoreline wetting: snap the field to the camera and hand the shader its
  // origin/extent before the globals upload; the compute pass records below.
  shore_wetting_active_ = settings_.shore_wetting &&
                          shore_wetting_.available() && !path_trace &&
                          !interior && scene_has_water;
  if (shore_wetting_active_) {
    shore_wetting_.BeginFrame(view.camera.eye);
    shore_wetting_.FieldParams(globals.shore_field);
    globals.flags |= kFrameFlagShoreWetting;
  }
  // Physically-based water material + crest-SSS tunables ([water] settings),
  // read by water.ps and (for the caustic depth fade) mesh.ps/mesh_rt.ps.
  for (u32 i = 0; i < 3; ++i)
    globals.water_absorption[i] = settings_.water_absorption[i];
  globals.water_absorption[3] = settings_.water_absorption_scale;
  globals.water_material[0] = settings_.water_transmission;
  globals.water_material[1] = settings_.water_refl_foam_gain;
  globals.water_material[2] = settings_.water_sss_intensity;
  globals.water_material[3] = settings_.water_sss_exponent;
  globals.water_caustics[0] = settings_.water_caustic_intensity;
  globals.water_caustics[1] = settings_.water_rest_height;
  globals.water_caustics[2] = settings_.water_caustic_depth_fade;
  // Underwater caustics: gated on an actual water surface (scene_has_water),
  // not on the interaction field -- the field defaults on in every scene, and
  // keying caustics off it painted the sun modulation onto dry ground below
  // y=0.
  water_caustics_active_ = settings_.water_caustics &&
                           water_caustics_.available() && !path_trace &&
                           !interior && scene_has_water;
  if (water_caustics_active_)
    globals.flags |= kFrameFlagWaterCaustics;
  // Skin blood-flow dynamics: advance the arterial pulse phase from the clock
  // and hand the perfusion/tension drivers to the skin pixel shader. The app
  // sets heart rate / global perfusion (exertion, blush, pallor) via settings.
  if (settings_.skin_dynamics) {
    constexpr f32 kTwoPi = 6.28318530718f;
    f32 phase = static_cast<f32>(time_seconds_) * settings_.skin_heart_rate * kTwoPi;
    globals.skin_dynamics[0] = std::fmod(phase, kTwoPi);
    globals.skin_dynamics[1] = std::clamp(settings_.skin_perfusion, -0.5f, 0.5f);
    globals.skin_dynamics[2] = settings_.skin_pulse_amplitude;
    globals.skin_dynamics[3] = settings_.skin_tension_gain;
    globals.flags |= kFrameFlagSkinDynamics;
  }
  // Hybrid ReSTIR DI decision happens before the globals upload below; the
  // graph passes record later under the same flag.
  bool restir_active = settings_.restir_di && rt_available_ &&
                       restir_di_.available() && !path_trace &&
                       light_count > 0 && raytracing_ &&
                       raytracing_->tlas(tlas_slot);
  if (restir_active)
    globals.flags |= kFrameFlagRestirDi;
  {
    // Froxel slicing: exponential view-z between the near plane and 500 m.
    constexpr f32 kNear = 0.1f, kFar = 500.0f;
    f32 scale = static_cast<f32>(kClusterSlices) / std::log2(kFar / kNear);
    globals.cluster_params[0] = scale;
    globals.cluster_params[1] = -std::log2(kNear) * scale;
    globals.cluster_params[2] =
        static_cast<f32>(render_width_) / static_cast<f32>(kClusterTilesX);
    globals.cluster_params[3] =
        static_cast<f32>(render_height_) / static_cast<f32>(kClusterTilesY);
  }
  std::memcpy(frame.globals.mapped, &globals, sizeof(globals));
  prev_view_proj_ = view_proj;
  has_prev_frame_ = true;

  // Skinning palette for every skinned draw this frame, read by device address.
  if (!view.bone_matrices.empty() && frame.bone_palette.mapped) {
    u32 count = std::min<u32>(static_cast<u32>(view.bone_matrices.size()),
                              kMaxFrameBones);
    std::memcpy(frame.bone_palette.mapped, view.bone_matrices.data(),
                count * sizeof(Mat4));
  }
  // Active morph target weights for every morphed draw, read by device address.
  if (!view.morph_weights.empty() && frame.morph_weights.mapped) {
    u32 count = std::min<u32>(static_cast<u32>(view.morph_weights.size()),
                              kMaxFrameMorphWeights);
    std::memcpy(frame.morph_weights.mapped, view.morph_weights.data(),
                count * sizeof(MorphWeight));
  }

  ResourceHandle scene_color =
      graph_.CreateTexture({.name = "scene_color",
                            .format = kSceneColorFormat,
                            .width = render_width_,
                            .height = render_height_});
  // The g-buffer aux targets only exist for the raster path; the path tracer
  // writes scene_color directly, so leaving them uncreated keeps the transient
  // pool from allocating images no pass touches.
  ResourceHandle motion = kInvalidResource, depth = kInvalidResource;
  ResourceHandle normals = kInvalidResource, depth_export = kInvalidResource;
  ResourceHandle shadow_atlas = kInvalidResource;
  if (csm_active) {
    shadow_atlas = graph_.CreateTexture({.name = "shadow_atlas",
                                         .format = ShadowPass::kAtlasFormat,
                                         .width = shadow_.atlas_width(),
                                         .height = shadow_.atlas_height()});
  }
  if (!path_trace) {
    motion = graph_.CreateTexture({.name = "motion",
                                   .format = kMotionFormat,
                                   .width = render_width_,
                                   .height = render_height_});
    depth = graph_.CreateTexture({.name = "depth",
                                  .format = kDepthFormat,
                                  .width = render_width_,
                                  .height = render_height_});
    normals = graph_.CreateTexture({.name = "normals",
                                    .format = kNormalFormat,
                                    .width = render_width_,
                                    .height = render_height_});
    // Raw reversed z exported by the prepass; every depth consumer samples
    // this so the real depth attachment never changes layout mid frame
    // (sampling round trips corrupt its compression metadata on nvidia).
    depth_export = graph_.CreateTexture({.name = "depth_export",
                                         .format = Format::kR32Float,
                                         .width = render_width_,
                                         .height = render_height_});
  }
  // Skin diffuse export: third scene-pass attachment, diffused by the
  // screen-space sss blur after the opaque pass (raster path only).
  ResourceHandle skin_diffuse = kInvalidResource;
  if (!path_trace) {
    skin_diffuse =
        graph_.CreateTexture({.name = "skin_diffuse",
                              .format = MeshPipeline::kSkinDiffuseFormat,
                              .width = render_width_,
                              .height = render_height_});
  }

  // kMsaa: multisampled twins for the geometry window (prepass + scene). The
  // plain handles above keep flowing to every downstream consumer as the
  // resolved single-sampled versions; only the two geometry passes and the
  // resolve/depth-rebuild passes below touch these.
  ResourceHandle geom_depth = depth, geom_normals = normals,
                 geom_motion = motion, geom_depth_export = depth_export,
                 geom_scene = scene_color, geom_skin = skin_diffuse;
  if (msaa) {
    geom_depth = graph_.CreateTexture({.name = "depth_ms",
                                       .format = kDepthFormat,
                                       .width = render_width_,
                                       .height = render_height_,
                                       .samples = msaa_samples});
    geom_normals = graph_.CreateTexture({.name = "normals_ms",
                                         .format = kNormalFormat,
                                         .width = render_width_,
                                         .height = render_height_,
                                         .samples = msaa_samples});
    geom_motion = graph_.CreateTexture({.name = "motion_ms",
                                        .format = kMotionFormat,
                                        .width = render_width_,
                                        .height = render_height_,
                                        .samples = msaa_samples});
    geom_depth_export = graph_.CreateTexture({.name = "depth_export_ms",
                                              .format = Format::kR32Float,
                                              .width = render_width_,
                                              .height = render_height_,
                                              .samples = msaa_samples});
    geom_scene = graph_.CreateTexture({.name = "scene_color_ms",
                                       .format = kSceneColorFormat,
                                       .width = render_width_,
                                       .height = render_height_,
                                       .samples = msaa_samples});
    geom_skin =
        graph_.CreateTexture({.name = "skin_diffuse_ms",
                              .format = MeshPipeline::kSkinDiffuseFormat,
                              .width = render_width_,
                              .height = render_height_,
                              .samples = msaa_samples});
  }

  // Virtual texturing: drain last frame's page requests, stream pages, and
  // record this frame's atlas/indirection uploads + the feedback copy/reset.
  if (!path_trace)
    virtual_texture_.AddToGraph(graph_, frame_index_);

  // FFT ocean: evolve the spectrum and rebuild the displacement/normal maps
  // the water shaders sample this frame.
  if (fft_ocean_active)
    ocean_.AddToGraph(graph_, static_cast<f32>(time_seconds_));

  // The persistent foam/ripple field runs later, after the prepass, so its
  // local-interaction phase can read the opaque depth (see below, past the
  // depth export). It still samples the FFT ocean foam recorded just above.

  // Shoreline wetting: update the world-space wet field this frame. Follows the
  // ocean pass so it can read the freshly-built displacement map when active.
  if (shore_wetting_active_) {
    ShoreWetting::Params sp;
    sp.camera_eye = view.camera.eye;
    sp.time = static_cast<f32>(time_seconds_);
    sp.dt = view.frame_delta_seconds;
    sp.drying_time = settings_.shore_drying_time;
    for (u32 i = 0; i < 4; ++i)
      sp.island[i] = settings_.shore_island[i];
    sp.fft_active = fft_ocean_active;
    sp.ocean_displacement =
        fft_ocean_active ? ocean_.displacement_view() : TextureView{};
    shore_wetting_.AddToGraph(graph_, sp);
  }

  // Underwater caustics: refract a sun-ray grid through the surface onto a
  // reference receiver plane and build the energy-conserving caustic map the
  // opaque scene pass samples. Follows the ocean pass so it can read the fresh
  // displacement/normal maps when the FFT ocean is active.
  if (water_caustics_active_) {
    WaterCaustics::Params cp;
    cp.sun_travel =
        Normalize(Vec3{globals.sun_direction[0], globals.sun_direction[1],
                       globals.sun_direction[2]});
    cp.time = static_cast<f32>(time_seconds_);
    cp.rest_height = settings_.water_rest_height;
    cp.receiver_depth = settings_.water_caustic_receiver_depth;
    cp.fft_active = fft_ocean_active;
    cp.ocean_displacement =
        fft_ocean_active ? ocean_.displacement_view() : TextureView{};
    cp.ocean_normal =
        fft_ocean_active ? ocean_.normal_foam_view() : TextureView{};
    water_caustics_.AddToGraph(graph_, cp);
  }

  // Aurora contribution to the environment bake: the CPU-side night factor
  // (the same smoothstep sky.ps applies to its screen-space copy) zeroes the
  // term whenever the sun is up, so a daylight scene never re-bakes for it.
  f32 env_aurora = 0.0f;
  if (settings_.weather.aurora && !interior) {
    f32 night = settings_.night;
    if (night < 0.0f) { // legacy fallback: infer from the sun's elevation
      f32 to_sun_y = -applied_sun_direction_.y;
      night = std::clamp((0.04f - to_sun_y) / 0.14f, 0.0f, 1.0f);
      night = night * night * (3.0f - 2.0f * night);
    }
    env_aurora =
        settings_.weather.aurora_intensity * std::clamp(night, 0.0f, 1.0f);
  }
  // An active aurora writhes: refresh the cubemap whenever its 0.4 s animation
  // step ticks over, so the curtains also move in the IBL and reflections. The
  // moment it fades to zero the environment re-bakes once more, so switching
  // the aurora off does not leave the last green bake in the IBL forever.
  constexpr f64 kAuroraBakeStep = 0.4;
  if (env_aurora > 0.0f &&
      std::floor(time_seconds_ / kAuroraBakeStep) !=
          std::floor((time_seconds_ - view.frame_delta_seconds) /
                     kAuroraBakeStep)) {
    environment_dirty_ = true;
  }
  if (env_aurora <= 0.0f && prev_env_aurora_ > 0.0f)
    environment_dirty_ = true;
  prev_env_aurora_ = env_aurora;
  if (environment_dirty_ && (settings_.ibl || settings_.sky)) {
    environment_dirty_ = false;
    Vec3 env_sun = applied_sun_direction_;
    f32 env_intensity = applied_sun_intensity_;
    Vec3 env_color = applied_sun_color_;
    f32 env_time = static_cast<f32>(time_seconds_);
    graph_.AddPass(
        "env_update", [](RenderGraph::PassBuilder &) {},
        [this, env_sun, env_intensity, env_color, env_aurora,
         env_time](PassContext &ctx) {
          environment_->RecordUpdate(*ctx.cmd, env_sun, env_intensity,
                                     env_color, env_aurora, env_time);
        });
  }

  // RCGI contributes to the TLAS build only in hardware mode; the software SDF
  // path has no ray query and (on non-ray-query devices) no raytracing context.
  if (rt_shadows || rtao_active || ddgi_active ||
      (rcgi_active && !rcgi_software) || water_pipeline_active ||
      reflections_active || path_trace || fog_active || precip_rt) {
    // Solid-angle instance culling + distance LOD are realtime-only shaping of
    // the ray-traced scene; the path tracer keeps every instance at LOD0 for
    // reference correctness. RX_RT_CULL / RX_RT_LOD_NEAR gate them
    // independently.
    const bool rt_cull_on = RtCullOpt && !path_trace;
    // RT LOD is independent of the raster force_lod0_for_tlas knob (which keeps
    // the rasterizer at LOD0 so screen-referenced rays self-intersect cleanly):
    // rays get LOD0 inside RX_RT_LOD_NEAR and coarsen past it. Path tracing
    // keeps LOD0 everywhere. RX_RT_LOD_NEAR <= 0 disables coarsening.
    const bool rt_lod_on = !path_trace && RtLodNear > 0.0f;
    const f32 lod_near = RtLodNear;
    rt_cull_.Configure(rt_cull_on, RtCullStart, RtCullAngle);
    rt_cull_.BeginFrame(view.camera.eye);
    const Vec3 eye = view.camera.eye;
    // Model-space bounding sphere of a mesh as a Vec3 center.
    auto mesh_center = [](const GpuMesh &m) {
      return Vec3{m.bounds_center[0], m.bounds_center[1], m.bounds_center[2]};
    };
    // Distance from the camera to an instance's world-space bounding centre.
    auto center_distance = [&](const GpuMesh &m, const Mat4 &t) {
      const Vec3 c = mesh_center(m);
      const f32 *mm = t.m;
      const f32 wx = mm[0] * c.x + mm[4] * c.y + mm[8] * c.z + mm[12];
      const f32 wy = mm[1] * c.x + mm[5] * c.y + mm[9] * c.z + mm[13];
      const f32 wz = mm[2] * c.x + mm[6] * c.y + mm[10] * c.z + mm[14];
      const f32 dx = wx - eye.x, dy = wy - eye.y, dz = wz - eye.z;
      return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    // Resolves the RT LOD for one instance: LOD0 inside the near radius (raster
    // and rays agree there, avoiding the self-intersection disparity the AC
    // Shadows team calls out), a coarser LOD past it. Fills custom_index/lod
    // for the TLAS instance, lazily building the LOD BLAS + record on first
    // need and falling back to LOD0 when the mesh has no usable geometry at
    // that LOD.
    auto select_rt = [&](u64 key, GpuMesh &m, const Mat4 &t, u32 &out_lod,
                         u32 &out_index) {
      out_lod = 0;
      out_index = m.bindless_index;
      if (!rt_lod_on || m.lods.empty())
        return;
      const f32 dist = center_distance(m, t);
      if (dist <= lod_near)
        return;
      const u32 lod = SelectLod(m, dist);
      if (lod == 0)
        return;
      const u32 idx = EnsureLodRtGeometry(key, m, lod);
      if (idx == BindlessRegistry::kInvalidIndex)
        return; // keep LOD0
      out_lod = lod;
      out_index = idx;
    };

    base::Vector<RayTracingContext::Instance> instances;
    instances.reserve(view.draws.size() + instances_.instance_count());
    for (const DrawItem &item : view.draws) {
      GpuMesh *mesh = meshes_.find(item.mesh);
      // no_rt grass-like fill stays out of the realtime tlas; when the path
      // tracer is active it joins with a path-trace-only instance mask, so
      // realtime rays (shadows/RTAO/reflections/fog/water) skip it either way:
      // they trace with RX_RAY_MASK_REALTIME.
      if (!mesh || mesh->all_blend || (mesh->no_rt && !path_trace))
        continue;
      // Per-draw instances are frustum-culled and few, so test solid angle
      // inline. no_rt fill is path-trace-only (never culled: culling is off
      // under the path tracer).
      if (rt_cull_on && !mesh->no_rt &&
          !rt_cull_.DrawVisible(item.transform, mesh_center(*mesh),
                                mesh->bounds_radius))
        continue;
      u8 mask = mesh->no_rt
                    ? static_cast<u8>(kRayMaskPathTrace)
                    : static_cast<u8>(kRayMaskRealtime | kRayMaskPathTrace);
      u32 lod = 0, index = mesh->bindless_index;
      if (!mesh->no_rt)
        select_rt(item.mesh, *mesh, item.transform, lod, index);
      instances.push_back({.mesh_key = item.mesh,
                           .custom_index = index,
                           .mask = mask,
                           .lod = lod,
                           .transform = item.transform});
      // Vegetation stand-in: a second instance on the opaque-approximation
      // BLAS, masked kRayMaskApprox so only the realtime diffuse/AO/shadow rays
      // hit it. Only at LOD0: distant LODs are built force-opaque and need no
      // stand-in.
      if (mesh->rt_approx && lod == 0) {
        instances.push_back({.mesh_key = item.mesh,
                             .custom_index = mesh->rt_approx_bindless,
                             .mask = static_cast<u8>(kRayMaskApprox),
                             .approx = true,
                             .transform = item.transform});
      }
    }
    const base::Vector<InstanceStore::Group> &groups = instances_.groups();
    for (u32 gi = 0; gi < groups.size(); ++gi) {
      const InstanceStore::Group &group = groups[gi];
      if (!group.alive)
        continue;
      GpuMesh *mesh = meshes_.find(group.mesh);
      if (!mesh || mesh->all_blend || (mesh->no_rt && !path_trace))
        continue;
      const u8 mask =
          mesh->no_rt ? static_cast<u8>(kRayMaskPathTrace)
                      : static_cast<u8>(kRayMaskRealtime | kRayMaskPathTrace);
      // Time-sliced solid-angle sweep for the (potentially thousands of) static
      // instances in this group; the persistent bitmask drives inclusion every
      // frame while only a slice is re-tested. no_rt fill is path-trace-only,
      // so culling never applies to it (path tracing disables culling).
      const bool group_cull = rt_cull_on && !mesh->no_rt;
      const base::Vector<u8> *visible =
          group_cull ? &rt_cull_.UpdateGroup(
                           gi, group.generation, group.revision,
                           {group.transforms.data(), group.transforms.size()},
                           mesh_center(*mesh), mesh->bounds_radius)
                     : nullptr;
      for (u32 ii = 0; ii < group.transforms.size(); ++ii) {
        if (visible && !(*visible)[ii])
          continue;
        const Mat4 &transform = group.transforms[ii];
        u32 lod = 0, index = mesh->bindless_index;
        if (!mesh->no_rt)
          select_rt(group.mesh, *mesh, transform, lod, index);
        instances.push_back({.mesh_key = group.mesh,
                             .custom_index = index,
                             .mask = mask,
                             .lod = lod,
                             .transform = transform});
        if (mesh->rt_approx && lod == 0) {
          instances.push_back({.mesh_key = group.mesh,
                               .custom_index = mesh->rt_approx_bindless,
                               .mask = static_cast<u8>(kRayMaskApprox),
                               .approx = true,
                               .transform = transform});
        }
      }
    }
    // Grow the TLAS now, on the build thread, so the record-time BuildTlas
    // never stalls the device or frees buffers mid command list (which races
    // the frame ring and corrupts the image). Spikes here when two worlds
    // stream in.
    if (raytracing_->ReserveTlas(tlas_build_slot,
                                 static_cast<u32>(instances.size()))) {
      graph_.AddPass(
          "tlas_build",
          [async_tlas](RenderGraph::PassBuilder &b) {
            if (async_tlas)
              b.Async(); // build next frame's slot on the compute queue
          },
          [this, tlas_build_slot, frame_index = frame_index_,
           instances = std::move(instances)](PassContext &ctx) {
            raytracing_->BuildTlas(*ctx.cmd, tlas_build_slot, frame_index,
                                   instances);
          });
    }
  }

  // DDGI right after the TLAS (its only same-frame dependency): flagged async
  // it forks onto the compute queue here and overlaps everything up to the
  // join before its first consumer (the reflection trace / scene pass).
  bool ddgi_async = ddgi_active && !path_trace && settings_.async_compute &&
                    device_->caps().async_compute;
  if (ddgi_active && !path_trace) {
    ddgi_->AddToGraph(graph_, *raytracing_, tlas_slot, view.camera.eye,
                      applied_sun_direction_, applied_sun_intensity_,
                      applied_sun_color_, frame_index_, ddgi_async);
  }

  // SDF clipmap composition (RX_SDF): min-blend the frame's instance SDFs into
  // the camera-following clipmap. This is the software-trace world side and is
  // independent of ray tracing (the whole point). One clip recomposited per
  // frame plus any clip that snapped this frame. Composed BEFORE the RCGI world
  // pass so the software probe trace reads this frame's clipmap (there is no
  // tlas_build to anchor after in software mode). Both use the kGeneral manual-
  // barrier discipline, so submission order is execution order on the queue.
  if (sdf_clipmap_ && sdf_available_) {
    base::Vector<SdfClipmap::Instance> sdf_instances;
    sdf_instances.reserve(view.draws.size() + instances_.instance_count());
    for (const DrawItem &item : view.draws) {
      if (sdf_scene_->Find(item.mesh))
        sdf_instances.push_back({item.mesh, item.transform});
    }
    for (const InstanceStore::Group &group : instances_.groups()) {
      if (!group.alive || !sdf_scene_->Find(group.mesh))
        continue;
      for (const Mat4 &transform : group.transforms) {
        sdf_instances.push_back({.mesh_key = group.mesh,
                                 .transform = transform,
                                 .bounded_quality = true});
      }
    }
    sdf_clipmap_->AddComposeToGraph(graph_, *sdf_scene_,
                                    std::move(sdf_instances), view.camera.eye,
                                    frame_index_);
  }

  // RCGI world side: cascaded light grid + spatial-hash radiance cache +
  // irradiance cascades. On RT hardware it can fork onto the async compute
  // queue and overlap up to the JoinAsync before its first consumer (the M2
  // gather). In software mode it traces the SDF clipmap (no tlas) on the main
  // timeline.
  bool rcgi_async = rcgi_world && !rcgi_software && settings_.async_compute &&
                    device_->caps().async_compute;
  if (rcgi_world) {
    light_grid_.AddToGraph(graph_, frame.lights, light_count, view.camera.eye,
                           frame_index_, rcgi_async);
    rcgi_->SetInteriorVolumes(interior_volumes_,
                              frame_index_); // game interior bounds
    rcgi_->set_gather_scale(
        static_cast<u32>(RcgiGatherScaleOpt));   // item 23: half/quarter res
    rcgi_->set_denoise_mask(RcgiDenoiseMaskOpt); // item 22: cross-class mask
    RcgiSystem::FrameConfig rcgi_cfg;
    rcgi_cfg.authored_interior = settings_.interior;
    rcgi_cfg.interior = settings_.interior && RcgiInteriorOpt;
    rcgi_cfg.interior_ambient = settings_.interior_ambient;
    rcgi_cfg.relocate = RcgiRelocateOpt;
    rcgi_cfg.classify =
        RcgiInteriorOpt; // volume classification shares the interior gate
    rcgi_cfg.probe_ao = RcgiProbeAoOpt;
    // Interior cells author their own directional (XCLL/LGTM) and suppress the
    // sky sun; feed RCGI the same authored interior sun the raster path
    // selected (above) instead of applied_sun_*, or an outdoor sun leaks bounce
    // onto an interior with zero directional intensity.
    Vec3 rcgi_sun_dir = settings_.interior
                            ? Normalize(settings_.interior_directional_dir)
                            : applied_sun_direction_;
    f32 rcgi_sun_int = settings_.interior
                           ? settings_.interior_directional_intensity
                           : applied_sun_intensity_;
    Vec3 rcgi_sun_col = settings_.interior
                            ? settings_.interior_directional_color
                            : applied_sun_color_;
    rcgi_->AddToGraph(graph_, raytracing_.get(), tlas_slot, light_grid_,
                      frame.lights, view.camera.eye, rcgi_sun_dir, rcgi_sun_int,
                      rcgi_sun_col, frame_index_, rcgi_cfg, rcgi_async,
                      rcgi_software ? sdf_clipmap_.get() : nullptr);
  }

  // The path tracer takes over the whole frame: it writes scene_color directly
  // and skips the entire raster path (g-buffer, gi, transparency, aa).
  ResourceHandle lit = scene_color;
  if (path_trace) {
    PathTracer::Frame pt;
    pt.inv_view_proj = globals.inv_view_proj;
    pt.view_proj = view_proj;
    pt.prev_view_proj =
        globals.prev_view_proj; // last frame's, set before the overwrite below
    pt.camera_pos = view.camera.eye;
    pt.sun_direction = settings_.sun_direction;
    pt.sun_intensity = settings_.sun_intensity;
    pt.sun_color = settings_.sun_color;
    pt.sun_radius = settings_.sun_angular_radius;
    pt.frame_index = frame_index_;
    f32 sig = settings_.sun_intensity + settings_.sun_color.x * 3.0f +
              settings_.sun_color.y * 5.0f + settings_.sun_color.z * 7.0f;
    bool moved =
        std::memcmp(&view_proj, &pt_prev_view_proj_, sizeof(Mat4)) != 0;
    bool lit_changed = sig != pt_prev_sig_;
    bool scene_changed = scene_revision_ != pt_prev_scene_revision_;
    bool denoised_path = false;

    // Gameplay reconstruction renderer: own 1-spp gbuffer + temporal
    // accumulation
    // + a-trous denoise + composite. Separate from the brute-force reference
    // and the NRD path. Reference always wins (screenshots); else recon if
    // selected.
    bool recon_path =
        settings_.path_trace_recon && !settings_.path_trace_reference;
    if (recon_path) {
      // Lazily allocate the recon history targets on first use: the mode is off
      // by default and the buffers are large, so they are not created up front.
      recon_path_tracer_.Resize(*device_, {render_width_, render_height_});
      bool rr_active = false;
#if defined(RX_HAS_DLSS)
      // Ray reconstruction replaces the SVGF chain when its snippet loads;
      // lazy-init mirrors the recon targets above.
      if (settings_.path_trace_rr && !rr_init_attempted_) {
        rr_init_attempted_ = true;
        if (!rr_.Initialize(*device_, {render_width_, render_height_})) {
          RX_INFO("dlss-rr unavailable, recon uses the in-tree svgf denoiser");
        }
      }
      rr_active = settings_.path_trace_rr && rr_.available();
#endif
      ReconPathTracer::Frame rf;
      rf.inv_view_proj = globals.inv_view_proj;
      rf.view_proj = view_proj;
      rf.prev_view_proj = globals.prev_view_proj;
      rf.camera_pos = view.camera.eye;
      rf.sun_direction = settings_.sun_direction;
      rf.sun_intensity = settings_.sun_intensity;
      rf.sun_color = settings_.sun_color;
      rf.sun_radius = settings_.sun_angular_radius;
      rf.pixel_spread = 2.0f * std::tan(view.camera.fov_y * 0.5f) /
                        static_cast<f32>(render_height_);
      rf.spp = settings_.path_trace_spp;
      rf.frame_index = frame_index_;
      // Reset on first frame, on (re)activation, AND when switching into recon
      // from another path-trace mode (its ping-pong history was never written
      // by the reference/NRD paths). Never on the day/night drift.
      rf.reset =
          first_frame || !pt_was_active_ || pt_prev_mode_ != 2 || scene_changed;
      rf.current_weight_min = settings_.path_trace_recon_weight;
      rf.max_history = settings_.path_trace_accum;
      rf.atrous_passes = settings_.path_trace_recon_atrous;
      rf.debug_mode = settings_.path_trace_recon_debug;
      // Modes 8/9 visualize the restir reservoir (M / W): the spatial pass
      // substitutes the heatmap, the composite renders it as raw lighting.
      if (rf.debug_mode >= 8) {
        rf.restir = true;
      }
      rf.restir = settings_.path_trace_restir;
      rf.restir_di = settings_.path_trace_restir_di;
      rf.lights = frame.lights;
      rf.light_count = light_count;
      rf.fog = settings_.fog;
      rf.fog_density = settings_.fog_density;
      rf.fog_height_falloff = settings_.fog_height_falloff;
      rf.fog_base_height = settings_.fog_base_height;
      rf.fog_anisotropy = settings_.fog_anisotropy;
#if defined(RX_HAS_DLSS)
      if (rr_active) {
        ReconPathTracer::ExternalInputs ext;
        recon_path_tracer_.AddToGraph(
            graph_, *raytracing_, tlas_slot, bindless_->set(),
            environment_->sky_view(), environment_->sampler(), scene_color, rf,
            &ext);
        RrDenoiser::Frame rrf;
        rrf.world_to_view = view_mat;
        rrf.view_to_clip = proj;
        rrf.frame_delta_ms = view.frame_delta_seconds * 1000.0f;
        rrf.reset = rf.reset;
        rr_.AddToGraph(graph_,
                       {ext.color, ext.depth, ext.motion, ext.normals_rough,
                        ext.diffuse_albedo, ext.specular_albedo},
                       scene_color, rrf);
      } else
#endif
      {
        (void)rr_active;
        recon_path_tracer_.AddToGraph(
            graph_, *raytracing_, tlas_slot, bindless_->set(),
            environment_->sky_view(), environment_->sampler(), scene_color, rf);
      }
    }
#if defined(RX_HAS_NRD)
    if (!recon_path && nrd_.available() && !settings_.path_trace_reference) {
      // Playable: spp lighting samples, then NRD's REBLUR_DIFFUSE reprojects
      // history across camera motion (no full reset), so the view stays clean
      // while moving. More spp = lower input variance = less shimmer.
      denoised_path = true;
      pt.spp = settings_.path_trace_spp;
      // Ray-cone spread for texture lod: vertical fov radians per pixel.
      pt.pixel_spread = 2.0f * std::tan(view.camera.fov_y * 0.5f) /
                        static_cast<f32>(render_height_);
      PathTracer::GbufferTargets t;
      auto guide = [&](const char *name, Format format) {
        return graph_.CreateTexture({.name = name,
                                     .format = format,
                                     .width = render_width_,
                                     .height = render_height_});
      };
      t.radiance_hitdist =
          guide("pt_radiance", NrdDenoiser::kDiffuseRadianceFormat);
      t.normal_roughness =
          guide("pt_normal_roughness", NrdDenoiser::kNormalRoughnessFormat);
      t.viewz = guide("pt_viewz", NrdDenoiser::kViewZFormat);
      t.motion = guide("pt_motion", kMotionFormat);
      t.albedo = guide("pt_albedo", kSceneColorFormat);
      t.background = guide("pt_background", kSceneColorFormat);
      path_tracer_.AddGbufferPass(graph_, *raytracing_, tlas_slot,
                                  bindless_->set(), environment_->sky_view(),
                                  environment_->sampler(), t, pt);

      NrdDenoiser::FrameSettings fs;
      fs.view_to_clip = proj;
      fs.view_to_clip_prev = prev_proj_;
      fs.world_to_view = view_mat;
      fs.world_to_view_prev = prev_view_;
      fs.jitter[0] = fs.jitter[1] =
          0.0f; // the path tracer shoots un-jittered rays
      fs.jitter_prev[0] = fs.jitter_prev[1] = 0.0f;
      fs.sun_direction = sun;
      fs.frame_index = frame_index_;
      fs.diffuse_accumulated_frames = settings_.path_trace_accum;
      // Restart ONLY on activation / first frame. NOT on lighting change: the
      // day/night cycle nudges the sun every frame, so resetting on that would
      // restart accumulation every frame and the image would never denoise (it
      // would stay 1-spp grainy forever). NRD tracks gradual lighting changes
      // through its own temporal accumulation + antilag instead. Also reset
      // when switching into the NRD path from another mode (stale reprojection
      // history).
      fs.reset =
          first_frame || !pt_was_active_ || pt_prev_mode_ != 1 || scene_changed;
      nrd_.SetFrame(fs);
      ResourceHandle denoised = nrd_.DenoiseDiffuse(
          graph_, t.normal_roughness, t.viewz, t.motion, t.radiance_hitdist);
      path_tracer_.AddCompositePass(graph_, denoised, t.albedo, t.background,
                                    scene_color);

      // The raster path stores these for NRD only when it runs; keep them
      // current for the next path-traced frame's motion vectors and
      // reprojection.
      prev_proj_ = proj;
      prev_view_ = view_mat;
      prev_jitter_[0] = prev_jitter_[1] = 0.0f;
    }
#endif
    if (!denoised_path && !recon_path) {
      // Reference: brute-force accumulation, hard reset on any motion = ground
      // truth. Also reset when switching into reference from another mode.
      pt.reset = !pt_was_active_ || moved || lit_changed || scene_changed ||
                 pt_prev_mode_ != 0;
      path_tracer_.AddToGraph(graph_, *raytracing_, tlas_slot, bindless_->set(),
                              environment_->sky_view(), environment_->sampler(),
                              scene_color, pt);
    }
    pt_prev_view_proj_ = view_proj;
    pt_prev_sig_ = sig;
    pt_prev_scene_revision_ = scene_revision_;
    pt_was_active_ = true;
    pt_prev_mode_ = recon_path ? 2 : (denoised_path ? 1 : 0);
  } else {
    pt_was_active_ = false;
    pt_prev_mode_ = -1;

    // Precipitation sky occlusion: a top-down "what can see the sky" depth map
    // gating the volumetric rain/snow, its splashes and the surface wetness /
    // snow accumulation (dry under bridges, splashes on roofs). Rendered early
    // so every consumer this frame samples fresh cover; re-rendered only when
    // the camera crosses an anchor cell or on a slow cadence.
    {
      const WeatherSettings &weather = settings_.weather;
      const bool weather_marks = weather.precipitation > 0.0f ||
                                 weather.wetness > 0.0f ||
                                 weather.snow_cover > 0.0f;
      // Not gated on `volumetric`: that flag only picks 3D versus screen-space
      // falling precipitation, while the surface wetness / snow passes need the
      // sky-visibility map either way (dry strips under bridges).
      precip_occlusion_active_ = weather_marks &&
                                 precip_occlusion_.available() &&
                                 !settings_.interior && !path_trace;
      if (precip_occlusion_active_) {
        precip_occlusion_.BeginFrame(view.camera.eye, frame_index_);
        precip_occlusion_.AddToGraph(
            graph_, [this, &frame, &view](CommandList &cmd,
                                          const Mat4 &occl_view_proj) {
              RecordDepthOnlyScene(cmd, occl_view_proj, frame, view);
            });
      }
    }

    u32 shadow_slot = frame_index_ % 2;
    if (csm_active) {
      Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
      Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
      Vec3 up = Cross(right, fwd);
      f32 shadow_aspect =
          static_cast<f32>(render_width_) / static_cast<f32>(render_height_);
      shadow_.Update(view.camera.eye, fwd, right, up, view.camera.fov_y,
                     shadow_aspect, settings_.sun_direction, shadow_slot);
      graph_.AddPass(
          "shadow_cascades",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Write(shadow_atlas, ResourceUsage::kDepthAttachment);
          },
          [this, shadow_atlas, &frame, &view](PassContext &ctx) {
            TextureView atlas = ctx.graph->image(shadow_atlas).view;
            shadow_.Render(*ctx.cmd, atlas,
                           [this, &frame, &view](CommandList &cmd,
                                                 const Mat4 &light_view_proj) {
                             RecordDepthOnlyScene(cmd, light_view_proj, frame,
                                                  view);
                           });
          });
    }

    // Local light shadow faces: depth-only renders with a light-radius culled
    // draw list, into the persistent atlas the cluster loop samples.
    if (local_shadows_active_) {
      graph_.AddPass(
          "local_shadows", [](RenderGraph::PassBuilder &) {},
          [this, &frame, &view](PassContext &ctx) {
            local_shadows_.Render(
                *ctx.cmd, shadow_.local_pipeline(),
                [this, &frame, &view](CommandList &cmd,
                                      const LocalShadows::Face &face) {
                  BindingSetHandle bound_material{};
                  // LocalShadows::Render bound the passed masked static
                  // pipeline.
                  PipelineHandle bound_pipeline = shadow_.local_pipeline();
                  for (const DrawItem &item : view.draws) {
                    const GpuMesh *mesh = meshes_.find(item.mesh);
                    if (!mesh || mesh->all_blend ||
                        (mesh->no_rt && !mesh->skinned &&
                         !mesh->dynamic_vertices))
                      continue;
                    // Sphere cull against the light's influence: local shadows
                    // only ever see casters inside the radius.
                    Vec3 wc = TransformPoint(item.transform,
                                             {mesh->bounds_center[0],
                                              mesh->bounds_center[1],
                                              mesh->bounds_center[2]});
                    const f32 *m = item.transform.m;
                    f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                    f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
                    f32 sz =
                        std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
                    f32 wr =
                        mesh->bounds_radius * std::max(sx, std::max(sy, sz));
                    Vec3 d{wc.x - face.light_pos.x, wc.y - face.light_pos.y,
                           wc.z - face.light_pos.z};
                    f32 reach = face.light_radius + wr;
                    if (wr > 0.0f &&
                        d.x * d.x + d.y * d.y + d.z * d.z > reach * reach)
                      continue;

                    bool draw_skinned =
                        mesh->skinned && item.skin_offset >= 0 &&
                        static_cast<bool>(shadow_.local_skinned_pipeline());
                    cmd.PushConstants(&item.transform, sizeof(Mat4),
                                      sizeof(Mat4));
                    cmd.BindVertexBuffer(0, mesh->vertices);
                    if (draw_skinned) {
                      cmd.BindVertexBuffer(1, mesh->skinning);
                      struct {
                        u64 bone_address;
                        u32 skin_offset;
                        u32 pad;
                      } skin{frame.bone_palette.address,
                             static_cast<u32>(item.skin_offset), 0};
                      cmd.PushConstants(&skin, sizeof(skin), 2 * sizeof(Mat4));
                    }
                    cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
                    for (const GpuSubmesh &submesh : mesh->submeshes) {
                      if (submesh.blend)
                        continue;
                      // Depth-only for opaque casters, alpha-test only for
                      // masked.
                      PipelineHandle pipeline =
                          draw_skinned
                              ? shadow_.local_skinned_pipeline(
                                    submesh.alpha_mask)
                              : shadow_.local_pipeline(submesh.alpha_mask);
                      if (!(pipeline == bound_pipeline)) {
                        cmd.BindPipeline(pipeline);
                        bound_pipeline = pipeline;
                      }
                      if (submesh.alpha_mask) {
                        BindingSetHandle material =
                            material_system_->set(submesh.material);
                        if (!(material == bound_material)) {
                          cmd.BindSet(0, material);
                          bound_material = material;
                        }
                      }
                      cmd.DrawIndexed(submesh.index_count, 1,
                                      submesh.index_offset, 0, 0);
                    }
                  }
                  for (const InstanceStore::Group &group :
                       instances_.groups()) {
                    if (!group.alive)
                      continue;
                    const GpuMesh *mesh = meshes_.find(group.mesh);
                    if (!mesh || mesh->all_blend ||
                        (mesh->no_rt && !mesh->dynamic_vertices))
                      continue;
                    const Vec3 delta = group.bounds_center - face.light_pos;
                    const f32 reach = face.light_radius + group.bounds_radius;
                    if (group.cullable && Dot(delta, delta) > reach * reach)
                      continue;
                    f32 face_planes[5][4];
                    ExtractFrustumPlanes(face.view_proj, face_planes);
                    if (group.cullable &&
                        SphereOutsideFrustum(face_planes, group.bounds_center,
                                             group.bounds_radius))
                      continue;
                    cmd.BindVertexBuffer(0, mesh->vertices);
                    cmd.BindVertexBuffer(1, group.buffer);
                    cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
                    for (const GpuSubmesh &submesh : mesh->submeshes) {
                      if (submesh.blend)
                        continue;
                      const PipelineHandle pipeline =
                          shadow_.local_instanced_pipeline(submesh.alpha_mask);
                      if (!(pipeline == bound_pipeline)) {
                        cmd.BindPipeline(pipeline);
                        bound_pipeline = pipeline;
                      }
                      if (submesh.alpha_mask) {
                        const BindingSetHandle material =
                            material_system_->set(submesh.material);
                        if (!(material == bound_material)) {
                          cmd.BindSet(0, material);
                          bound_material = material;
                        }
                      }
                      cmd.DrawIndexed(submesh.index_count,
                                      static_cast<u32>(group.transforms.size()),
                                      submesh.index_offset, 0, 0);
                    }
                  }
                });
          });
    }

    // GPU-driven culling: build one indirect command per opaque submesh and one
    // cull instance per opaque mesh, in the exact order the prepass/scene draw
    // loops walk view.draws, then let a compute pass zero the culled
    // instanceCounts.
    u32 cull_slot = frame_index_ % 2;
    gpu_cull_.ResizeDepth(*device_, render_width_, render_height_);
    const GpuBuffer &cull_commands = gpu_cull_.command_buffer(cull_slot);
    u32 cull_instance_count = 0;
    {
      GpuCull::Instance *insts = gpu_cull_.instances(cull_slot);
      GpuCull::Command *cmds = gpu_cull_.commands(cull_slot);
      u32 cmd_total = 0;
      for (const DrawItem &item : view.draws) {
        const GpuMesh *mesh = meshes_.find(item.mesh);
        if (!mesh || mesh->all_blend)
          continue;
        if (cull_instance_count >= GpuCull::kMaxInstances ||
            cmd_total >= GpuCull::kMaxCommands)
          break;
        GpuCull::Instance &inst = insts[cull_instance_count];
        inst.model = item.transform;
        inst.bounds[0] = mesh->bounds_center[0];
        inst.bounds[1] = mesh->bounds_center[1];
        inst.bounds[2] = mesh->bounds_center[2];
        inst.bounds[3] = mesh->bounds_radius;
        inst.first_cmd = cmd_total;
        inst.cull_disabled = (mesh->skinned || mesh->morph_target_count > 0 ||
                              mesh->bounds_radius <= 0.0f)
                                 ? 1u
                                 : 0u;
        inst.pad = 0;

        // Default to lod 0 (finest): we stream and render the highest detail
        // the game ships. Distance-based downgrade is opt-in
        // (settings_.distance_lod). Skinned, morphed and rt-shaded meshes
        // always stay on lod 0 (their bounds deform / the morph deltas index
        // lod 0 vertices / the tlas is built from lod 0). The submesh count
        // matches lod 0 so the prepass/scene draw loops issue one indirect per
        // submesh.
        bool fixed_lod = !settings_.distance_lod || mesh->skinned ||
                         mesh->morph_target_count > 0 ||
                         (force_lod0_for_tlas && !mesh->no_rt);
        u32 lod = 0;
        if (!fixed_lod) {
          Vec3 wc = TransformPoint(item.transform, {mesh->bounds_center[0],
                                                    mesh->bounds_center[1],
                                                    mesh->bounds_center[2]});
          Vec3 d = view.camera.eye - wc;
          lod = SelectLod(*mesh, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
        }
        const base::Vector<GpuSubmesh> &lod_subs =
            lod == 0 ? mesh->submeshes : mesh->lods[lod - 1].submeshes;
        i32 vtx_off =
            lod == 0 ? 0 : static_cast<i32>(mesh->lods[lod - 1].vertex_offset);

        u32 mesh_cmds = 0;
        u32 k = 0;
        for (const GpuSubmesh &submesh : mesh->submeshes) {
          if (!submesh.blend) {
            if (cmd_total >= GpuCull::kMaxCommands)
              break;
            const GpuSubmesh &s = k < lod_subs.size() ? lod_subs[k] : submesh;
            cmds[cmd_total] = {s.index_count, 1u, s.index_offset, vtx_off, 0u};
            ++cmd_total;
            ++mesh_cmds;
          }
          ++k;
        }
        inst.cmd_count = mesh_cmds;
        if (mesh_cmds > 0)
          ++cull_instance_count;
      }
      cull_total_commands_ = cmd_total;
      // This slot's previous cull finished (the frame fence was waited on), so
      // its count is valid; read it before AddToGraph resets the buffer.
      cull_visible_ =
          settings_.gpu_culling ? gpu_cull_.last_visible(cull_slot) : cmd_total;
    }
    bool cull_occlusion =
        settings_.gpu_culling && settings_.gpu_occlusion && has_prev_frame_;
    ResourceHandle cull_hiz = cull_occlusion
                                  ? gpu_cull_.BuildHiZ(graph_, cull_slot)
                                  : kInvalidResource;
    const f32 cull_proj_scale[2] = {proj.m[0], proj.m[5]};
    gpu_cull_.AddToGraph(graph_, view_proj, globals.prev_view_proj,
                         cull_proj_scale, view.camera.eye, cull_instance_count,
                         settings_.gpu_culling, cull_occlusion, cull_hiz,
                         cull_slot);

    bool grass_active = false;
    if (settings_.procedural_grass && view.grass_domain &&
        procedural_grass_.EnsureSampleCount(*device_, msaa_samples)) {
      ProceduralGrass::Frame grass_frame;
      grass_frame.view_proj = globals.view_proj;
      grass_frame.prev_view_proj = globals.prev_view_proj;
      grass_frame.camera_pos = view.camera.eye;
      grass_frame.sun_direction = sun;
      grass_frame.sun_color = {globals.sun_color[0], globals.sun_color[1],
                               globals.sun_color[2]};
      grass_frame.sun_intensity = globals.sun_direction[3];
      grass_frame.ambient = globals.sun_color[3];
      grass_frame.time = static_cast<f32>(time_seconds_);
      grass_frame.delta_time = view.frame_delta_seconds;
      grass_frame.jitter[0] = globals.jitter[0];
      grass_frame.jitter[1] = globals.jitter[1];
      grass_frame.wind_speed = settings_.weather.wind_speed;
      grass_frame.wind_yaw = settings_.weather.wind_yaw;
      grass_frame.gustiness = settings_.weather.gustiness;
      // World width of one pixel at unit view depth, for the sub-pixel blade
      // width clamp. m[5] is negative under the reversed-Z Y-flip projection.
      grass_frame.pixel_scale =
          proj.m[5] != 0.0f
              ? 2.0f / (std::fabs(proj.m[5]) * static_cast<f32>(render_height_))
              : 0.0f;
      grass_active = procedural_grass_.Prepare(
          *view.grass_domain,
          {view.grass_interactions.data(), view.grass_interactions.size()},
          grass_frame, frame_slot);
      if (grass_active) procedural_grass_.AddGeneration(graph_, frame_slot);
    }

    // Mesh-shader opaque path: drawn this frame if enabled and supported. The
    // task stage reuses the same hi-z the raster cull built (last frame's) for
    // instance occlusion; ms_occ carries the projection scale + hi-z size (z=0
    // disables it). The meshlet pipelines stay single-sampled, so the path sits
    // out kMsaa.
    const bool ms_active =
        settings_.mesh_shader_lod && mesh_pipeline_->has_mesh_shader() && !msaa;
    const bool ms_occlude = ms_active && cull_occlusion;
    f32 ms_occ[4] = {0, 0, 0, 0};
    if (ms_occlude) {
      ms_occ[0] = proj.m[0];
      ms_occ[1] = proj.m[5];
      ms_occ[2] = static_cast<f32>(gpu_cull_.hiz_width());
      ms_occ[3] = static_cast<f32>(gpu_cull_.hiz_height());
    }
    // Frustum planes for the cpu-side skip of off-screen mesh-shader draws.
    f32 ms_planes[5][4];
    ExtractFrustumPlanes(view_proj, ms_planes);

    // Draws every mesh-shader-eligible mesh; shared by the prepass and scene
    // sub-passes (material binding differs via the bind callbacks).
    auto draw_meshlet_instances = [this, &view, &ms_occ, &ms_planes,
                                   &frame](PassContext &ctx) {
      BindingSetHandle bound{};
      for (const DrawItem &item : view.draws) {
        const GpuMesh *mesh = meshes_.find(item.mesh);
        if (!mesh || mesh->all_blend || !mesh->has_meshlets)
          continue;
        MeshShaderPush push{};
        push.model = item.transform;
        push.prev_model = item.prev_transform;
        push.meshlets_address = mesh->meshlets.address;
        push.meshlet_vertices_address = mesh->meshlet_vertices.address;
        push.meshlet_triangles_address = mesh->meshlet_triangles.address;
        push.vertices_address = mesh->vertices.address;
        push.bounds[0] = mesh->bounds_center[0];
        push.bounds[1] = mesh->bounds_center[1];
        push.bounds[2] = mesh->bounds_center[2];
        push.bounds[3] = mesh->bounds_radius;
        push.occlusion[0] = ms_occ[0];
        push.occlusion[1] = ms_occ[1];
        push.occlusion[2] = ms_occ[2];
        push.occlusion[3] = ms_occ[3];
        // Distance lod pick; the task stage dispatches the chosen lod range.
        Vec3 ms_wc = TransformPoint(item.transform, {mesh->bounds_center[0],
                                                     mesh->bounds_center[1],
                                                     mesh->bounds_center[2]});
        Vec3 ms_d = view.camera.eye - ms_wc;
        // Cpu frustum skip: a conservative world radius (bounds scaled by the
        // largest transform axis) lets off-screen instances cost no dispatch.
        const f32 *m = item.transform.m;
        f32 sx = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        f32 sy = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        f32 sz = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        f32 ms_radius = mesh->bounds_radius * std::max(sx, std::max(sy, sz));
        if (ms_radius > 0.0f &&
            SphereOutsideFrustum(ms_planes, ms_wc, ms_radius))
          continue;
        u32 ms_lod =
            SelectLod(*mesh, std::sqrt(ms_d.x * ms_d.x + ms_d.y * ms_d.y +
                                       ms_d.z * ms_d.z));
        const base::Vector<GpuSubmesh> &ms_subs =
            ms_lod == 0 ? mesh->submeshes : mesh->lods[ms_lod - 1].submeshes;
        for (const GpuSubmesh &submesh : ms_subs) {
          if (submesh.blend || submesh.meshlet_count == 0)
            continue;
          BindingSetHandle material = material_system_->set(submesh.material);
          if (!(material == bound)) {
            mesh_pipeline_->BindMeshMaterial(*ctx.cmd, material);
            bound = material;
          }
          push.meshlet_offset = submesh.meshlet_offset;
          push.meshlet_count = submesh.meshlet_count;
          mesh_pipeline_->DrawMeshlets(*ctx.cmd, push);
        }
      }
    };

    graph_.AddPass(
        "prepass",
        [&](RenderGraph::PassBuilder &builder) {
          builder.Write(geom_normals, ResourceUsage::kColorAttachment);
          builder.Write(geom_motion, ResourceUsage::kColorAttachment);
          builder.Write(geom_depth_export, ResourceUsage::kColorAttachment);
          builder.Write(geom_depth, ResourceUsage::kDepthAttachment);
          if (ms_occlude)
            builder.Read(cull_hiz, ResourceUsage::kSampledTaskMesh);
        },
        [this, geom_normals, geom_motion, geom_depth_export, geom_depth,
         cull_commands, &frame, &view, ms_active, ms_occlude, cull_hiz,
         globals_set, update_globals_set, frame_slot, draw_meshlet_instances,
         view_proj, force_lod0_for_tlas, grass_active,
         msaa_samples](PassContext &ctx) {
          // First globals-set user this frame: write uniform + tlas + hi-z
          // once.
          update_globals_set(ctx, ms_occlude ? cull_hiz : kInvalidResource,
                             ms_active,
                             /*want_tlas=*/true);

          ColorAttachment colors[3];
          colors[0] = {.view = ctx.graph->image(geom_normals).view};
          colors[1] = {.view = ctx.graph->image(geom_motion).view};
          colors[2] = {.view = ctx.graph->image(geom_depth_export).view};
          DepthAttachment depth_attachment{
              .view = ctx.graph->image(geom_depth).view,
              .clear = 0.0f}; // reversed z clears to far = 0
          ctx.cmd->BeginRendering(
              {.extent = {render_width_, render_height_},
               .colors = {colors, 3},
               .depth = &depth_attachment,
               .shading_rate = vrs_active_ ? vrs_.rate_view() : TextureView{}});

          // Mesh-shader sub-pass: static opaque meshes, cluster-culled on the
          // gpu.
          if (ms_active) {
            mesh_pipeline_->BindMeshPrepass(*ctx.cmd, globals_set);
            draw_meshlet_instances(ctx);
          }

          // Raster sub-pass: skinned / non-meshlet meshes via gpu-culled
          // indirect draws. Meshes already drawn by the mesh shader are skipped
          // but still advance the cull index so it stays aligned with the cull
          // build order.
          environment_->WriteEnvSet(
              env_prepass_sets_[frame_slot], TextureView{}, nullptr,
              TextureView{}, GpuBuffer{}, 0, TextureView{}, TextureView{},
              GpuBuffer{}, 0, TextureView{}, GpuBuffer{}, GpuBuffer{},
              GpuBuffer{}, GpuBuffer{}, TextureView{}, GpuBuffer{},
              TextureView{}, TextureView{}, TextureView{}, TextureView{},
              GpuBuffer{}, TextureView{}, TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : TextureView{});
          mesh_pipeline_->BindPrepass(*ctx.cmd, globals_set,
                                      env_prepass_sets_[frame_slot]);
          BindingSetHandle bound_material{};
          bool skinned_bound = false;
          bool masked_bound =
              false;              // BindPrepass bound the opaque static variant
          u32 cull_cmd_index = 0; // matches the cull build order
          for (const DrawItem &item : view.draws) {
            const GpuMesh *mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            // Stay within the commands the cull build wrote; past that the
            // indirect buffer holds no valid command and reading it renders as
            // garbage.
            if (cull_cmd_index >= cull_total_commands_)
              break;
            bool ms_handled = ms_active && mesh->has_meshlets;
            bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
            if (!ms_handled) {
              MeshPushConstants push{.model = item.transform,
                                     .prev_model = item.prev_transform};
              if (mesh->terrain_lod) {
                std::memcpy(push.detail_rect, view.detail_rect,
                            sizeof(push.detail_rect));
              }
              if (draw_skinned && item.skin_offset >= 0) {
                push.bone_address = frame.bone_palette.address;
                push.skin_offset = static_cast<u32>(item.skin_offset);
              }
              if (mesh->morph_target_count > 0 && item.morph_offset >= 0 &&
                  item.morph_count > 0) {
                push.morph_delta_address = mesh->morph_deltas.address;
                push.morph_weight_address = frame.morph_weights.address;
                push.morph_first = static_cast<u32>(item.morph_offset);
                push.morph_count = item.morph_count;
                push.morph_vertex_count = mesh->vertex_count;
              }
              mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
            }
            for (const GpuSubmesh &submesh : mesh->submeshes) {
              if (submesh.blend)
                continue; // transparency owns its own depth
              if (cull_cmd_index >= cull_total_commands_)
                break; // partial-mesh boundary
              if (!ms_handled) {
                // Masked submeshes take the alpha-test variant; opaque ones
                // keep the discard-free fragment so the draw keeps early-Z.
                if (draw_skinned != skinned_bound ||
                    submesh.alpha_mask != masked_bound) {
                  mesh_pipeline_->SetPrepassVariant(*ctx.cmd, draw_skinned,
                                                    submesh.alpha_mask);
                  skinned_bound = draw_skinned;
                  masked_bound = submesh.alpha_mask;
                }
                BindingSetHandle material =
                    material_system_->set(submesh.material);
                if (!(material == bound_material)) {
                  mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                  bound_material = material;
                }
                ctx.cmd->DrawIndexedIndirect(
                    cull_commands, cull_cmd_index * GpuCull::kCommandStride, 1,
                    GpuCull::kCommandStride);
              }
              ++cull_cmd_index;
            }
          }
          f32 instance_planes[5][4];
          ExtractFrustumPlanes(view_proj, instance_planes);
          for (const InstanceStore::Group &group : instances_.groups()) {
            if (!group.alive ||
                (group.cullable &&
                 SphereOutsideFrustum(instance_planes, group.bounds_center,
                                      group.bounds_radius))) {
              continue;
            }
            const GpuMesh *mesh = meshes_.find(group.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            const u32 lod =
                !settings_.distance_lod || (force_lod0_for_tlas && !mesh->no_rt)
                    ? 0
                    : SelectLod(*mesh,
                                InstanceGroupDistance(group, view.camera.eye));
            const base::Vector<GpuSubmesh> &submeshes =
                lod == 0 ? mesh->submeshes : mesh->lods[lod - 1].submeshes;
            const i32 vertex_offset =
                lod == 0 ? 0
                         : static_cast<i32>(mesh->lods[lod - 1].vertex_offset);
            MeshPushConstants push{};
            const GpuBuffer &previous =
                group.previous_buffer ? group.previous_buffer : group.buffer;
            mesh_pipeline_->DrawInstances(*ctx.cmd, *mesh, group.buffer,
                                          previous, push);
            for (const GpuSubmesh &submesh : submeshes) {
              if (submesh.blend)
                continue;
              mesh_pipeline_->SetInstancedPrepass(*ctx.cmd, submesh.alpha_mask);
              const BindingSetHandle material =
                  material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              ctx.cmd->DrawIndexed(submesh.index_count,
                                   static_cast<u32>(group.transforms.size()),
                                   submesh.index_offset, vertex_offset, 0);
            }
          }
          if (grass_active)
            procedural_grass_.DrawPrepass(*ctx.cmd, frame_slot, msaa_samples);
          ctx.cmd->EndRendering();
        });

    // kMsaa: resolve the prepass guides to single-sampled (sample 0 - averaging
    // would invent phantom depths/normals at silhouettes) and rebuild the 1x
    // hardware depth every post-resolve raster pass tests against. Motion
    // resolves after the scene pass instead (the sky still writes it there).
    if (msaa) {
      graph_.AddPass(
          "msaa_guides",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Read(geom_normals, ResourceUsage::kSampledCompute);
            builder.Read(geom_depth_export, ResourceUsage::kSampledCompute);
            builder.Write(normals, ResourceUsage::kStorageWrite);
            builder.Write(depth_export, ResourceUsage::kStorageWrite);
          },
          [this, geom_normals, geom_depth_export, normals,
           depth_export](PassContext &ctx) {
            struct {
              u32 width;
              u32 height;
            } push{render_width_, render_height_};
            ctx.cmd->BindPipeline(msaa_resolve_pipeline_);
            ctx.cmd->BindTransient(
                0,
                {Bind::SampledView(0, ctx.graph->image(geom_normals).view),
                 Bind::SampledView(1, ctx.graph->image(geom_depth_export).view),
                 Bind::Storage(2, ctx.graph->image(normals)),
                 Bind::Storage(3, ctx.graph->image(depth_export))});
            ctx.cmd->Push(push);
            ctx.cmd->Dispatch2D({render_width_, render_height_});
          });
      graph_.AddPass(
          "msaa_depth_copy",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Read(depth_export, ResourceUsage::kSampledFragment);
            builder.Write(depth, ResourceUsage::kDepthAttachment);
          },
          [this, depth_export, depth](PassContext &ctx) {
            DepthAttachment depth_attachment{.view =
                                                 ctx.graph->image(depth).view,
                                             .load = LoadOp::kDontCare};
            ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                     .depth = &depth_attachment});
            ctx.cmd->BindPipeline(depth_copy_pipeline_);
            ctx.cmd->BindTransient(
                0, {Bind::SampledView(0, ctx.graph->image(depth_export).view)});
            ctx.cmd->Draw(3, 1, 0, 0);
            ctx.cmd->EndRendering();
          });
    }

    // Snapshot this frame's depth for next frame's occlusion test.
    if (settings_.gpu_culling && settings_.gpu_occlusion) {
      gpu_cull_.CopyDepth(graph_, depth_export, cull_slot);
    }

    // Persistent foam/ripple field: recenter+advect+decay the rings, step the
    // near-camera ripples, and inject crest foam + object wakes. Scheduled
    // here, after the prepass exports depth, so the local-interaction phase can
    // read it: it projects each ring-0 column into the frame, and where opaque
    // geometry crosses the waterline it rings the surface (no CPU disturbance
    // needed) and reflects ripples off the analytic island. Still ordered after
    // the FFT ocean (recorded earlier) so crest injection samples the fresh
    // foam map.
    if (water_field_active_) {
      WaterField::UpdateParams wf{};
      wf.camera_pos = view.camera.eye;
      wf.time = static_cast<f32>(time_seconds_);
      wf.dt = std::min(view.frame_delta_seconds,
                       1.0f / 30.0f); // clamp for stability
      wf.frame_slot = frame_slot;
      wf.fft_ocean = fft_ocean_active;
      wf.disturbances = view.water_disturbances.data();
      wf.disturbance_count = static_cast<u32>(view.water_disturbances.size());
      wf.interaction = settings_.water_interaction;
      // The analytic island is only a defined terrain source where the
      // shoreline wetting owns it; reflect ripples off it only then (mirrors
      // that gating).
      wf.obstacle = settings_.water_interaction && shore_wetting_active_ &&
                    WaterObstacleOpt;
      wf.view_proj = globals.view_proj;
      wf.inv_view_proj = globals.inv_view_proj;
      for (u32 i = 0; i < 4; ++i)
        wf.island[i] = settings_.shore_island[i];
      wf.water_level = 0.0f;
      wf.render_size[0] = static_cast<f32>(render_width_);
      wf.render_size[1] = static_cast<f32>(render_height_);
      water_field_.AddToGraph(
          graph_, wf,
          fft_ocean_active ? ocean_.normal_foam_view() : TextureView{},
          fft_ocean_active ? ocean_.displacement_view() : TextureView{},
          depth_export);
    }

    // Optional heightfield fluid solver (flowing water + lava). Scheduled here
    // alongside the water field; it steps its own domain textures and the
    // surface renderer samples them downstream. Sources bounded at 64.
    if (fluid_sim_active_) {
      FluidSim::UpdateParams fp{};
      fp.domain = view.fluid_domain;
      fp.dt = std::min(view.frame_delta_seconds, 1.0f / 30.0f);
      fp.frame_slot = frame_slot;
      fp.sources = view.fluid_sources.data();
      fp.source_count =
          std::min<u32>(static_cast<u32>(view.fluid_sources.size()), FluidSim::kMaxSources);
      fluid_sim_.AddToGraph(graph_, fp);
    }

    ResourceHandle ao = kInvalidResource;
    ResourceHandle sun_shadow = kInvalidResource;
    ResourceHandle spec_refl = kInvalidResource;
    ResourceHandle rcgi_irr = kInvalidResource;

    // RCGI screen side runs BEFORE the reflection trace so the reflection
    // ray-skip (item 16) can read the gather's denoised per-pixel diffuse SH.
    // M2 half-res SH final gather -> bilateral denoise -> full-res upscale +
    // temporal filter, writing the "rcgi_irradiance" transient the forward pass
    // folds in. `RX_RCGI_PROBES_ONLY=1` (and always in software mode) swaps in
    // the M1 per-pixel cascade resolve. The gather is the first consumer of the
    // (optionally async) world passes, so join the compute queue before it.
    // RCGI and DDGI are mutually exclusive, so this never races the DDGI join
    // used by the reflection/DDGI path below.
#if defined(RX_HAS_NRD)
    // The gather's denoised SH, captured for the specular ray-skip (reflections
    // are NRD-gated, so this is only consumed under RX_HAS_NRD).
    ResourceHandle refl_sh[3] = {kInvalidResource, kInvalidResource,
                                 kInvalidResource};
    Extent2D refl_sh_extent{};
#endif
    if (rcgi_world && depth_export != kInvalidResource &&
        normals != kInvalidResource) {
      if (rcgi_async) {
        graph_.AddPass(
            "async_join", [](RenderGraph::PassBuilder &b) { b.JoinAsync(); },
            [](PassContext &) {});
      }
      if (rcgi_probes_only ||
          !rcgi_->EnsureScreenResources({render_width_, render_height_})) {
        rcgi_irr = rcgi_->AddResolvePass(
            graph_, depth_export, normals, {render_width_, render_height_},
            globals.inv_view_proj, view.camera.eye, settings_.rcgi_intensity,
            frame_index_);
      } else {
        rcgi_irr = rcgi_->AddGatherChain(
            graph_, *raytracing_, tlas_slot, depth_export, normals, motion,
            {render_width_, render_height_}, globals.inv_view_proj,
            globals.prev_view_proj, view.camera.eye, settings_.rcgi_intensity,
            frame_index_, first_frame);
#if defined(RX_HAS_NRD)
        ResourceHandle sh[3];
        Extent2D e{};
        if (rcgi_->denoised_sh(sh, e)) {
          refl_sh[0] = sh[0];
          refl_sh[1] = sh[1];
          refl_sh[2] = sh[2];
          refl_sh_extent = e;
        }
#endif
      }
    }
#if defined(RX_HAS_NRD)
    if (nrd_ao || nrd_shadow) {
      // Shared NRD guides (normal+roughness, viewZ) and per-frame camera state
      // for the REBLUR ao and SIGMA sun-shadow denoisers.
      NrdDenoiser::Inputs nrd_inputs =
          nrd_.PrepareInputs(graph_, depth_export, normals, 0.1f);
      NrdDenoiser::FrameSettings fs;
      fs.view_to_clip = proj;
      fs.view_to_clip_prev = prev_proj_;
      fs.world_to_view = view_mat;
      fs.world_to_view_prev = prev_view_;
      fs.jitter[0] = jitter_x;
      fs.jitter[1] = jitter_y;
      fs.jitter_prev[0] = prev_jitter_[0];
      fs.jitter_prev[1] = prev_jitter_[1];
      fs.sun_direction = sun;
      fs.frame_index = frame_index_;
      fs.reset = first_frame;
      nrd_.SetFrame(fs);
      if (nrd_ao) {
        // RTAO traces a raw hit distance; REBLUR denoises it.
        ResourceHandle hitdist =
            rtao_.AddToGraph(graph_, *raytracing_, tlas_slot, depth_export,
                             normals, globals.inv_view_proj, frame_index_, 0.1f,
                             NrdDenoiser::kHitDistParams);
        ao = nrd_.DenoiseAo(graph_, nrd_inputs.normal_roughness,
                            nrd_inputs.view_z, motion, hitdist);
      }
      if (nrd_shadow) {
        // Trace a 1-spp cone-jittered sun visibility into SIGMA's penumbra
        // input, then denoise it into a clean screen-space sun shadow the
        // lighting samples (instead of the noisier inline trace the temporal
        // pass had to integrate).
        ResourceHandle penumbra = shadow_trace_.AddToGraph(
            graph_, *raytracing_, tlas_slot, depth_export,
            globals.inv_view_proj, sun, 0.1f, settings_.sun_angular_radius,
            globals.jitter[0], globals.jitter[1]);
        sun_shadow = nrd_.DenoiseShadow(graph_, nrd_inputs.normal_roughness,
                                        nrd_inputs.view_z, motion, penumbra);
        // Contact shadows: a short screen-space march folded into the denoised
        // shadow, restoring the sub-30cm grounding the 1-spp ray blurs away.
        graph_.AddPass(
            "contact_shadow",
            [&](RenderGraph::PassBuilder &b) {
              b.Write(sun_shadow, ResourceUsage::kStorageWrite);
              b.Read(depth_export, ResourceUsage::kSampledCompute);
            },
            [this, sun_shadow, depth_export, sun, view_proj = globals.view_proj,
             inv_view_proj = globals.inv_view_proj](PassContext &ctx) {
              struct ContactPush {
                Mat4 view_proj;
                Mat4 inv_view_proj;
                f32 sun_dir[3];
                f32 near_plane;
                u32 size[2];
                f32 range;
                f32 thickness;
                u32 steps;
                u32 frame_index;
                f32 pad[2];
              } p{};
              p.view_proj = view_proj;
              p.inv_view_proj = inv_view_proj;
              p.sun_dir[0] = sun.x;
              p.sun_dir[1] = sun.y;
              p.sun_dir[2] = sun.z;
              p.near_plane = 0.1f;
              p.size[0] = render_width_;
              p.size[1] = render_height_;
              p.range = 0.35f;
              p.thickness = 0.4f;
              p.steps = 12;
              p.frame_index = frame_index_;
              ctx.cmd->BindPipeline(contact_shadow_pipeline_);
              ctx.cmd->BindTransient(
                  0, {Bind::Storage(0, ctx.graph->image(sun_shadow)),
                      Bind::Sampled(1, ctx.graph->image(depth_export))});
              ctx.cmd->Push(p);
              ctx.cmd->Dispatch2D({render_width_, render_height_});
            });
        if (settings_.cloudscape && cloudscape_ready_ && !interior) {
          // Cloudscape ground shadows: the same textured density field the
          // march renders, so the shade tracks the formations that actually
          // occlude the sun (gaps stay lit, cores darken, wind advects both).
          Cloudscape::Frame sf;
          sf.inv_view_proj = globals.inv_view_proj;
          sf.frame_index = frame_index_;
          sf.jitter[0] = globals.jitter[0];
          sf.jitter[1] = globals.jitter[1];
          sf.sun_direction = settings_.sun_direction;
          sf.time = static_cast<f32>(time_seconds_);
          sf.controls = settings_.cloudscape_controls;
          sf.controls.wind_yaw = settings_.weather.wind_yaw;
          sf.controls.wind_speed = settings_.weather.wind_speed;
          cloudscape_.AddShadowToGraph(graph_, sun_shadow, depth_export,
                                       {render_width_, render_height_}, sf, 0.75f);
        } else if (settings_.clouds && !interior) {
          // Cloud shadows: the layer's optical depth along the sun ray darkens
          // the same denoised shadow the shading samples.
          graph_.AddPass(
              "cloud_shadow",
              [&](RenderGraph::PassBuilder &b) {
                b.Write(sun_shadow, ResourceUsage::kStorageWrite);
                b.Read(depth_export, ResourceUsage::kSampledCompute);
              },
              [this, sun_shadow, depth_export, sun,
               inv_view_proj = globals.inv_view_proj](PassContext &ctx) {
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
                  f32 wind_z; // z drift velocity, matches cloud_shadow.cs
                  f32 pad[3];
                } p{};
                p.inv_view_proj = inv_view_proj;
                p.sun_dir[0] = sun.x;
                p.sun_dir[1] = sun.y;
                p.sun_dir[2] = sun.z;
                p.near_plane = 0.1f;
                p.size[0] = render_width_;
                p.size[1] = render_height_;
                p.time = static_cast<f32>(time_seconds_);
                p.coverage = settings_.cloud_coverage;
                p.bottom = 1500.0f;
                p.top = 4200.0f;
                // Same drift velocity as clouds.cs, so the shadows track the
                // deck.
                p.wind = std::cos(settings_.weather.wind_yaw) *
                         settings_.weather.wind_speed;
                p.wind_z = std::sin(settings_.weather.wind_yaw) *
                           settings_.weather.wind_speed;
                p.strength = 0.75f;
                ctx.cmd->BindPipeline(cloud_shadow_pipeline_);
                ctx.cmd->BindTransient(
                    0, {Bind::Storage(0, ctx.graph->image(sun_shadow)),
                        Bind::Sampled(1, ctx.graph->image(depth_export))});
                ctx.cmd->Push(p);
                ctx.cmd->Dispatch2D({render_width_, render_height_});
              });
        }
      }
      if (ddgi_async && spec_refl_active) {
        // First DDGI consumer: the reflection trace samples the probe atlases.
        graph_.AddPass(
            "async_join", [](RenderGraph::PassBuilder &b) { b.JoinAsync(); },
            [](PassContext &) {});
      }
      if (spec_refl_active) {
        // 1-spp VNDF reflection radiance -> REBLUR_SPECULAR; the scene pass
        // samples the result instead of tracing an inline mirror ray.
        EnvironmentSystem::DdgiBinding refl_ddgi;
        if (ddgi_active)
          refl_ddgi = ddgi_->binding(frame_index_);
        ReflectionTrace::Frame rfl;
        rfl.inv_view_proj = globals.inv_view_proj;
        rfl.camera_pos = view.camera.eye;
        rfl.sun_direction = settings_.sun_direction;
        rfl.sun_intensity =
            settings_.sun_intensity + settings_.weather.lightning * 9.0f;
        rfl.sun_color = settings_.sun_color;
        rfl.roughness_cutoff = settings_.reflection_roughness_cutoff;
        rfl.frame_index = frame_index_;
        rfl.near_plane = 0.1f;
        rfl.hit_dist_params = NrdDenoiser::kHitDistParams;
        rfl.ddgi = ddgi_active;
        // Real-alpha any-hit only when the vegetation feature is on overall.
        rfl.veg_anyhit = RtVegOpt && RtVegAnyHitOpt;
        // Phase 4: half-res trace + upscale, roughness-scaled reach, one-step
        // fog, and diffuse-SH ray-skip (only when the RCGI gather SH is
        // available).
        rfl.half_res = ReflHalfOpt;
        rfl.fog = ReflFogOpt && fog_active;
        rfl.fog_density = settings_.fog_density;
        rfl.fog_height_falloff = settings_.fog_height_falloff;
        rfl.fog_base_height = settings_.fog_base_height;
        rfl.sh_skip = ReflShSkipOpt && refl_sh[0] != kInvalidResource;
        rfl.sh_skip_roughness = ReflShSkipRough;
        // Spec-bounce indirect diffuse: under RCGI the DDGI atlas is empty, so
        // the reflection hit reads the RCGI irradiance cascades (item 20b).
        // Bind the real cascades when RCGI is active, else environment
        // placeholders so the descriptor set is complete (kFlagRcgi stays clear
        // -> never sampled).
        ReflectionTrace::RcgiBinding refl_rcgi;
        RcgiSystem::IrradianceBinding rcgi_irr_bind;
        if (rcgi_active)
          rcgi_irr_bind = rcgi_->irradiance_binding(frame_index_);
        if (rcgi_irr_bind.valid) {
          refl_rcgi.irradiance = rcgi_irr_bind.irradiance;
          refl_rcgi.visibility = rcgi_irr_bind.visibility;
          refl_rcgi.globals = rcgi_irr_bind.globals;
          refl_rcgi.probe_meta = rcgi_irr_bind.probe_meta;
          refl_rcgi.interior_vols = rcgi_irr_bind.interior_vols;
          refl_rcgi.sampler = rcgi_irr_bind.sampler;
          refl_rcgi.in_general = true;
          refl_rcgi.active = true;
        } else {
          refl_rcgi.irradiance = environment_->black_view();
          refl_rcgi.visibility = environment_->black_view();
          refl_rcgi.globals = &environment_->dummy_volume();
          refl_rcgi.probe_meta = &environment_->dummy_storage();
          refl_rcgi.interior_vols = &environment_->dummy_storage();
          refl_rcgi.sampler = environment_->sampler();
        }
        ResourceHandle raw = reflection_trace_.AddToGraph(
            graph_, *raytracing_, tlas_slot, bindless_->set(), depth_export,
            normals, environment_->prefiltered_view(),
            ddgi_active ? refl_ddgi.irradiance
                        : environment_->black_array_view(),
            ddgi_active,
            ddgi_active ? refl_ddgi.volume : environment_->dummy_volume(),
            ddgi_active ? refl_ddgi.volume_size : 256, environment_->sampler(),
            {render_width_, render_height_}, refl_sh[0], refl_sh[1], refl_sh[2],
            refl_sh_extent, refl_rcgi, rfl);
        spec_refl = nrd_.DenoiseSpecular(graph_, nrd_inputs.normal_roughness,
                                         nrd_inputs.view_z, motion, raw);
      }
      prev_proj_ = proj;
      prev_view_ = view_mat;
      prev_jitter_[0] = jitter_x;
      prev_jitter_[1] = jitter_y;
    }
#endif
    if (ss_ao) {
      const f32 proj_scale[2] = {proj.m[0], proj.m[5]};
      ao =
          ssao_.AddToGraph(graph_, depth_export, normals, globals.inv_view_proj,
                           proj_scale, 0.1f, frame_index_);
    }

    // Hybrid ReSTIR DI: reservoir-resampled point/spot lights with one shadow
    // ray per pixel, shaded off the prepass G-buffer into screen-space targets
    // the forward pass folds back in (env slots 23/24, kFrameFlagRestirDi).
    RestirDi::Outputs restir_out;
    if (restir_active) {
      RestirDi::Frame rf;
      rf.inv_view_proj = globals.inv_view_proj;
      rf.camera_pos = view.camera.eye;
      rf.frame_index = frame_index_;
      rf.light_count = globals.light_count;
      rf.lights = frame.lights;
      rf.tlas_slot = tlas_slot;
      restir_out = restir_di_.AddToGraph(graph_, depth_export, normals, motion,
                                         *raytracing_,
                                         {render_width_, render_height_}, rf);
    }

    // Without the reflection trace the scene pass is DDGI's first consumer;
    // join the async queue before the lighting work leading into it.
    if (ddgi_async && spec_refl == kInvalidResource) {
      graph_.AddPass(
          "async_join", [](RenderGraph::PassBuilder &b) { b.JoinAsync(); },
          [](PassContext &) {});
    }

    // Froxel light culling: fixed slots per cluster, run every frame (a zero
    // light count still zeroes the counts the forward loop reads).
    graph_.AddPass(
        "light_cluster", [&](RenderGraph::PassBuilder &) {},
        [this, &frame, &view, light_count, decal_count,
         proj](PassContext &ctx) {
          struct ClusterPush {
            Mat4 view_mat;
            f32 screen[2];
            f32 near_plane;
            f32 slice_scale;
            f32 slice_bias;
            u32 light_count;
            f32 tan_half_fov_y;
            f32 aspect;
            u32 decal_count;
            f32 pad[3];
          } p{};
          p.view_mat = LookAt(view.camera.eye, view.camera.target, {0, 1, 0});
          p.screen[0] = static_cast<f32>(render_width_);
          p.screen[1] = static_cast<f32>(render_height_);
          p.near_plane = 0.1f;
          constexpr f32 kNear = 0.1f, kFar = 500.0f;
          p.slice_scale =
              static_cast<f32>(kClusterSlices) / std::log2(kFar / kNear);
          p.slice_bias = -std::log2(kNear) * p.slice_scale;
          p.light_count = light_count;
          p.tan_half_fov_y = std::tan(view.camera.fov_y * 0.5f);
          p.aspect = static_cast<f32>(render_width_) /
                     static_cast<f32>(render_height_);
          p.decal_count = decal_count;
          ctx.cmd->BindPipeline(light_cluster_pipeline_);
          ctx.cmd->BindTransient(
              0,
              {Bind::StorageBuffer(0, frame.lights, 0, frame.lights.size),
               Bind::StorageBuffer(1, cluster_counts_, 0, cluster_counts_.size),
               Bind::StorageBuffer(2, cluster_indices_, 0,
                                   cluster_indices_.size),
               Bind::StorageBuffer(3, frame.decals, 0, frame.decals.size),
               Bind::StorageBuffer(4, decal_cluster_indices_, 0,
                                   decal_cluster_indices_.size)});
          ctx.cmd->Push(p);
          ctx.cmd->Dispatch((kClusterCount + 63) / 64, 1, 1);
          ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite,
                                 BarrierScope::kGraphicsRead);
        });

    graph_.AddPass(
        "scene",
        [&](RenderGraph::PassBuilder &builder) {
          builder.Write(geom_scene, ResourceUsage::kColorAttachment);
          builder.Write(geom_motion, ResourceUsage::kColorAttachment);
          builder.Write(geom_skin, ResourceUsage::kColorAttachment);
          builder.Write(geom_depth, ResourceUsage::kDepthAttachment);
          if (ao != kInvalidResource)
            builder.Read(ao, ResourceUsage::kSampledFragment);
          if (sun_shadow != kInvalidResource)
            builder.Read(sun_shadow, ResourceUsage::kSampledFragment);
          if (spec_refl != kInvalidResource)
            builder.Read(spec_refl, ResourceUsage::kSampledFragment);
          if (csm_active)
            builder.Read(shadow_atlas, ResourceUsage::kSampledFragment);
          if (restir_active) {
            builder.Read(restir_out.diffuse, ResourceUsage::kSampledFragment);
            builder.Read(restir_out.spec, ResourceUsage::kSampledFragment);
          }
          if (rcgi_irr != kInvalidResource)
            builder.Read(rcgi_irr, ResourceUsage::kSampledFragment);
          if (ms_occlude)
            builder.Read(cull_hiz, ResourceUsage::kSampledTaskMesh);
        },
        [this, geom_scene, geom_motion, geom_skin, geom_depth, msaa, ao,
         sun_shadow, spec_refl, rcgi_irr, rcgi_world, use_rt_frag, ddgi_active,
         csm_active, shadow_slot, shadow_atlas, cull_commands, ms_active,
         globals_set, frame_slot, restir_active, restir_out, &frame, &view,
          draw_meshlet_instances, view_proj, force_lod0_for_tlas,
          grass_active, msaa_samples](PassContext &ctx) {
          BindingSetHandle env_set = env_scene_sets_[frame_slot];
          TextureView ao_view = ao != kInvalidResource
                                    ? ctx.graph->image(ao).view
                                    : TextureView{};
          TextureView rcgi_irr_view = rcgi_irr != kInvalidResource
                                          ? ctx.graph->image(rcgi_irr).view
                                          : TextureView{};
          TextureView sun_shadow_view = sun_shadow != kInvalidResource
                                            ? ctx.graph->image(sun_shadow).view
                                            : TextureView{};
          TextureView spec_refl_view = spec_refl != kInvalidResource
                                           ? ctx.graph->image(spec_refl).view
                                           : TextureView{};
          EnvironmentSystem::DdgiBinding ddgi_binding;
          if (ddgi_active)
            ddgi_binding = ddgi_->binding(frame_index_);
          // Inline reflection bounce reads the RCGI world cascades when RCGI is
          // active (matches the NRD reflection path and the kFrameFlagRcgi
          // gate); null -> placeholders. The atlases stay in GENERAL between
          // frames.
          EnvironmentSystem::RcgiWorldBinding rcgi_world_binding;
          RcgiSystem::IrradianceBinding rcgi_world_src;
          if (rcgi_world)
            rcgi_world_src = rcgi_->irradiance_binding(frame_index_);
          if (rcgi_world_src.valid) {
            rcgi_world_binding.irradiance = rcgi_world_src.irradiance;
            rcgi_world_binding.visibility = rcgi_world_src.visibility;
            rcgi_world_binding.globals = rcgi_world_src.globals;
            rcgi_world_binding.probe_meta = rcgi_world_src.probe_meta;
            rcgi_world_binding.interior_vols = rcgi_world_src.interior_vols;
          }
          environment_->WriteEnvSet(
              env_set, ao_view, ddgi_active ? &ddgi_binding : nullptr,
              csm_active ? ctx.graph->image(shadow_atlas).view : TextureView{},
              csm_active ? shadow_.cascade_buffer(shadow_slot) : GpuBuffer{},
              shadow_.cascade_buffer_size(), TextureView{}, sun_shadow_view,
              frame.lights, frame.lights.size, spec_refl_view, cluster_counts_,
              cluster_indices_, frame.decals, decal_cluster_indices_,
              decal_atlas_view_,
              local_shadows_active_ ? local_shadows_.face_buffer(frame_slot)
                                    : GpuBuffer{},
              local_shadows_active_ ? local_shadows_.atlas().view
                                    : TextureView{},
              decal_normal_atlas_view_,
              restir_active ? ctx.graph->image(restir_out.diffuse).view
                            : TextureView{},
              restir_active ? ctx.graph->image(restir_out.spec).view
                            : TextureView{},
              virtual_texture_.available() ? virtual_texture_.feedback_buffer()
                                           : GpuBuffer{},
              virtual_texture_.available() ? virtual_texture_.indirection_view()
                                           : TextureView{},
              virtual_texture_.available() ? virtual_texture_.atlas_view()
                                           : TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : TextureView{},
              TextureView{}, TextureView{},
              GpuBuffer{}, // water field rings: transparent pass only
              shore_wetting_active_ ? shore_wetting_.current_view()
                                    : TextureView{},
              water_caustics_active_ ? water_caustics_.current_view()
                                     : TextureView{},
              rcgi_irr_view,
              rcgi_world_src.valid ? &rcgi_world_binding : nullptr);

          ColorAttachment colors[3];
          colors[0] = {.view = ctx.graph->image(geom_scene).view,
                       .load = LoadOp::kClear,
                       .clear = {0.02f, 0.02f, 0.05f, 1.0f}};
          colors[1] = {.view = ctx.graph->image(geom_motion).view,
                       .load = LoadOp::kLoad}; // the prepass wrote motion
          colors[2] = {.view = ctx.graph->image(geom_skin).view,
                       .load = LoadOp::kClear,
                       .clear = {0.0f, 0.0f, 0.0f, 0.0f}};
          DepthAttachment depth_attachment{
              .view = ctx.graph->image(geom_depth).view,
              .load = LoadOp::kLoad}; // prepass depth, tested EQUAL
          ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                   .colors = {colors, 3},
                                   .depth = &depth_attachment});

          BindingSetHandle bindless_set =
              bindless_ ? bindless_->set() : BindingSetHandle{};

          // Mesh-shader sub-pass: static opaque meshes, finest lod,
          // cluster-culled.
          if (ms_active) {
            mesh_pipeline_->BindMeshScene(*ctx.cmd, globals_set, env_set,
                                          bindless_set, use_rt_frag);
            draw_meshlet_instances(ctx);
          }

          mesh_pipeline_->Bind(*ctx.cmd, globals_set, env_set, bindless_set,
                               use_rt_frag, settings_.wireframe);
          BindingSetHandle bound_material{};
          bool skinned_bound = false;
          u32 cull_cmd_index = 0; // matches the cull build + prepass order
          for (const DrawItem &item : view.draws) {
            const GpuMesh *mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            if (cull_cmd_index >= cull_total_commands_)
              break; // clamp to the built commands
            bool ms_handled = ms_active && mesh->has_meshlets;
            bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
            if (!ms_handled) {
              if (draw_skinned != skinned_bound) {
                mesh_pipeline_->SetSkinned(*ctx.cmd, draw_skinned, use_rt_frag,
                                           settings_.wireframe);
                skinned_bound = draw_skinned;
              }
              MeshPushConstants push{.model = item.transform,
                                     .prev_model = item.prev_transform};
              if (mesh->terrain_lod) {
                std::memcpy(push.detail_rect, view.detail_rect,
                            sizeof(push.detail_rect));
              }
              if (draw_skinned && item.skin_offset >= 0) {
                push.bone_address = frame.bone_palette.address;
                push.skin_offset = static_cast<u32>(item.skin_offset);
              }
              if (mesh->morph_target_count > 0 && item.morph_offset >= 0 &&
                  item.morph_count > 0) {
                push.morph_delta_address = mesh->morph_deltas.address;
                push.morph_weight_address = frame.morph_weights.address;
                push.morph_first = static_cast<u32>(item.morph_offset);
                push.morph_count = item.morph_count;
                push.morph_vertex_count = mesh->vertex_count;
              }
              push.tint_packed =
                  item.tint; // faction/team colour for skinned actors
              mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
            }
            for (const GpuSubmesh &submesh : mesh->submeshes) {
              if (submesh.blend)
                continue;
              if (cull_cmd_index >= cull_total_commands_)
                break; // partial-mesh boundary
              if (!ms_handled) {
                BindingSetHandle material =
                    material_system_->set(submesh.material);
                if (!(material == bound_material)) {
                  mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                  bound_material = material;
                }
                ctx.cmd->DrawIndexedIndirect(
                    cull_commands, cull_cmd_index * GpuCull::kCommandStride, 1,
                    GpuCull::kCommandStride);
              }
              ++cull_cmd_index;
            }
          }
          f32 instance_planes[5][4];
          ExtractFrustumPlanes(view_proj, instance_planes);
          for (const InstanceStore::Group &group : instances_.groups()) {
            if (!group.alive ||
                (group.cullable &&
                 SphereOutsideFrustum(instance_planes, group.bounds_center,
                                      group.bounds_radius))) {
              continue;
            }
            const GpuMesh *mesh = meshes_.find(group.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            const u32 lod =
                !settings_.distance_lod || (force_lod0_for_tlas && !mesh->no_rt)
                    ? 0
                    : SelectLod(*mesh,
                                InstanceGroupDistance(group, view.camera.eye));
            const base::Vector<GpuSubmesh> &submeshes =
                lod == 0 ? mesh->submeshes : mesh->lods[lod - 1].submeshes;
            const i32 vertex_offset =
                lod == 0 ? 0
                         : static_cast<i32>(mesh->lods[lod - 1].vertex_offset);
            mesh_pipeline_->SetInstanced(*ctx.cmd, use_rt_frag,
                                         settings_.wireframe);
            MeshPushConstants push{};
            const GpuBuffer &previous =
                group.previous_buffer ? group.previous_buffer : group.buffer;
            mesh_pipeline_->DrawInstances(*ctx.cmd, *mesh, group.buffer,
                                          previous, push);
            for (const GpuSubmesh &submesh : submeshes) {
              if (submesh.blend)
                continue;
              const BindingSetHandle material =
                  material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              ctx.cmd->DrawIndexed(submesh.index_count,
                                   static_cast<u32>(group.transforms.size()),
                                   submesh.index_offset, vertex_offset, 0);
            }
          }
          if (grass_active)
            procedural_grass_.DrawScene(*ctx.cmd, frame_slot, msaa_samples);
          // The sky pipeline is single-sampled; under kMsaa it draws in its own
          // pass right after the resolve instead (same attachments at 1x).
          if (settings_.sky && !settings_.interior && !msaa) {
            environment_->DrawSky(*ctx.cmd, globals_set);
          }
          ctx.cmd->EndRendering();
        });

    // kMsaa: average-resolve the scene color (this is the actual antialiasing),
    // motion (the geometry part; the sky adds its motion at 1x below) and the
    // skin-diffuse export, then draw the sky into the resolved targets with the
    // rebuilt 1x depth. Everything after this point runs exactly as in kNone.
    if (msaa) {
      graph_.AddPass(
          "msaa_resolve",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Read(geom_scene, ResourceUsage::kResolveSrc);
            builder.Read(geom_motion, ResourceUsage::kResolveSrc);
            builder.Read(geom_skin, ResourceUsage::kResolveSrc);
            builder.Write(scene_color, ResourceUsage::kResolveDst);
            builder.Write(motion, ResourceUsage::kResolveDst);
            builder.Write(skin_diffuse, ResourceUsage::kResolveDst);
          },
          [this, geom_scene, geom_motion, geom_skin, scene_color, motion,
           skin_diffuse](PassContext &ctx) {
            ctx.cmd->ResolveTexture(ctx.graph->image(geom_scene),
                                    ctx.graph->image(scene_color));
            ctx.cmd->ResolveTexture(ctx.graph->image(geom_motion),
                                    ctx.graph->image(motion));
            ctx.cmd->ResolveTexture(ctx.graph->image(geom_skin),
                                    ctx.graph->image(skin_diffuse));
          });
      if (settings_.sky && !settings_.interior) {
        graph_.AddPass(
            "msaa_sky",
            [&](RenderGraph::PassBuilder &builder) {
              builder.Write(scene_color, ResourceUsage::kColorAttachment);
              builder.Write(motion, ResourceUsage::kColorAttachment);
              builder.Write(skin_diffuse, ResourceUsage::kColorAttachment);
              builder.Write(depth, ResourceUsage::kDepthAttachment);
            },
            [this, scene_color, motion, skin_diffuse, depth,
             globals_set](PassContext &ctx) {
              ColorAttachment colors[3];
              colors[0] = {.view = ctx.graph->image(scene_color).view,
                           .load = LoadOp::kLoad};
              colors[1] = {.view = ctx.graph->image(motion).view,
                           .load = LoadOp::kLoad};
              colors[2] = {.view = ctx.graph->image(skin_diffuse).view,
                           .load = LoadOp::kLoad};
              DepthAttachment depth_attachment{
                  .view = ctx.graph->image(depth).view, .load = LoadOp::kLoad};
              ctx.cmd->BeginRendering(
                  {.extent = {render_width_, render_height_},
                   .colors = {colors, 3},
                   .depth = &depth_attachment});
              environment_->DrawSky(*ctx.cmd, globals_set);
              ctx.cmd->EndRendering();
            });
      }
    }

    // App scene passes (render/rhi hooks): record raw app GPU work into rx's
    // frame, depth-interleaved with rx geometry. Nothing added when unset.
    // `add_scene_hook` runs the callback inside a first-class graph pass with
    // the color/depth (and, for the opaque phase, depth-export) writes declared
    // so the graph barriers them; the callback opens its own dynamic-rendering
    // section.
    auto add_scene_hook =
        [&](const std::function<void(const SceneHookContext &)> &hook,
            ScenePhase phase, ResourceHandle color_h, ResourceHandle depth_h,
            ResourceHandle export_h, ResourceHandle motion_h) {
          const f32 jx = globals.jitter[0], jy = globals.jitter[1];
          const Mat4 prev_vp = globals.prev_view_proj;
          graph_.AddPass(
              phase == ScenePhase::kOpaque ? "app_scene_opaque"
                                           : "app_scene_transparent",
              [color_h, depth_h, export_h, motion_h](RenderGraph::PassBuilder &b) {
                b.Write(color_h, ResourceUsage::kColorAttachment);
                b.Write(depth_h, ResourceUsage::kDepthAttachment);
                if (export_h != kInvalidResource)
                  b.Write(export_h, ResourceUsage::kColorAttachment);
                if (motion_h != kInvalidResource)
                  b.Write(motion_h, ResourceUsage::kColorAttachment);
              },
              // `hook` is copied into the closure (the graph executes this
              // after add_scene_hook returns, so a reference to the parameter
              // would dangle).
              [this, hook, phase, color_h, depth_h, export_h, motion_h, proj,
               view_mat, view_proj, prev_vp, jx, jy, &view](PassContext &ctx) {
                SceneHookContext hc;
                hc.phase = phase;
                hc.cmd = ctx.cmd;
                hc.device = ctx.device;
                const GpuImage &color_img = ctx.graph->image(color_h);
                hc.color = &color_img;
                hc.color_view = color_img.view;
                hc.color_format = color_img.format;
                const GpuImage &depth_img = ctx.graph->image(depth_h);
                hc.depth = &depth_img;
                hc.depth_view = depth_img.view;
                hc.depth_format = depth_img.format;
                if (export_h != kInvalidResource) {
                  const GpuImage &export_img = ctx.graph->image(export_h);
                  hc.depth_export = &export_img;
                  hc.depth_export_view = export_img.view;
                  hc.depth_export_format = export_img.format;
                }
                if (motion_h != kInvalidResource) {
                  const GpuImage &motion_img = ctx.graph->image(motion_h);
                  hc.motion = &motion_img;
                  hc.motion_view = motion_img.view;
                  hc.motion_format = motion_img.format;
                }
                hc.extent = {render_width_, render_height_};
                hc.frame_slot = frame_index_ % kFramesInFlight;
                hc.frames_in_flight = kFramesInFlight;
                hc.view = view_mat;
                hc.proj = proj;
                hc.view_proj = view_proj;
                hc.prev_view_proj = prev_vp;
                hc.jitter[0] = jx;
                hc.jitter[1] = jy;
                hc.near_plane = 0.1f;
                hc.camera_pos = view.camera.eye;
                hook(hc);
              });
        };
    if (view.scene_opaque && !path_trace && depth != kInvalidResource) {
      add_scene_hook(view.scene_opaque, ScenePhase::kOpaque, scene_color, depth,
                     depth_export, motion);
    }

    // Screen-space subsurface scattering: separable per-channel diffusion of
    // the skin buffer the scene pass exported, folded back into the opaque
    // color before anything composites on top.
    if (settings_.sss) {
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
      auto fill_sss = [this, proj](SssPush &p) {
        p.size[0] = render_width_;
        p.size[1] = render_height_;
        p.inv_size[0] = 1.0f / static_cast<f32>(render_width_);
        p.inv_size[1] = 1.0f / static_cast<f32>(render_height_);
        p.near_plane = 0.1f;
        // The per-pixel scatter radius now rides the skin-diffuse alpha (derived
        // from each material's mean free path); width is a global artist
        // multiplier. 0.012 m is the historical reference radius = 1x.
        p.width = settings_.sss_width / 0.012f;
        // Pixels per meter at view depth 1. The projection bakes the vulkan
        // y-flip into m[5], so it is negative - take the magnitude.
        p.proj_scale =
            std::abs(proj.m[5]) * 0.5f * static_cast<f32>(render_height_);
        p.max_radius = 24.0f;
        p.strength =
            std::getenv("RX_SSS_DEBUG") ? -1.0f : 1.0f; // <0 = mask debug view
      };
      ResourceHandle sss_tmp =
          graph_.CreateTexture({.name = "sss_tmp",
                                .format = MeshPipeline::kSkinDiffuseFormat,
                                .width = render_width_,
                                .height = render_height_});
      graph_.AddPass(
          "sss_blur_h",
          [&](RenderGraph::PassBuilder &b) {
            b.Write(sss_tmp, ResourceUsage::kStorageWrite);
            b.Read(skin_diffuse, ResourceUsage::kSampledCompute);
            b.Read(depth_export, ResourceUsage::kSampledCompute);
          },
          [this, sss_tmp, skin_diffuse, depth_export,
           fill_sss](PassContext &ctx) {
            SssPush p{};
            fill_sss(p);
            p.dir[0] = 1.0f;
            p.composite = 0;
            ctx.cmd->BindPipeline(sss_pipeline_);
            ctx.cmd->BindTransient(
                0, {Bind::Storage(0, ctx.graph->image(sss_tmp)),
                    Bind::Combined(1, ctx.graph->image(skin_diffuse).view,
                                   sss_sampler_),
                    Bind::Combined(2, ctx.graph->image(depth_export).view,
                                   sss_sampler_),
                    Bind::Combined(3, ctx.graph->image(skin_diffuse).view,
                                   sss_sampler_)});
            ctx.cmd->Push(p);
            ctx.cmd->Dispatch2D({render_width_, render_height_});
          });
      graph_.AddPass(
          "sss_apply",
          [&](RenderGraph::PassBuilder &b) {
            b.Write(scene_color, ResourceUsage::kStorageWrite);
            b.Read(sss_tmp, ResourceUsage::kSampledCompute);
            b.Read(skin_diffuse, ResourceUsage::kSampledCompute);
            b.Read(depth_export, ResourceUsage::kSampledCompute);
          },
          [this, scene_color, sss_tmp, skin_diffuse, depth_export,
           fill_sss](PassContext &ctx) {
            SssPush p{};
            fill_sss(p);
            p.dir[1] = 1.0f;
            p.composite = 1;
            ctx.cmd->BindPipeline(sss_pipeline_);
            ctx.cmd->BindTransient(
                0, {Bind::Storage(0, ctx.graph->image(scene_color)),
                    Bind::Combined(1, ctx.graph->image(sss_tmp).view,
                                   sss_sampler_),
                    Bind::Combined(2, ctx.graph->image(depth_export).view,
                                   sss_sampler_),
                    Bind::Combined(3, ctx.graph->image(skin_diffuse).view,
                                   sss_sampler_)});
            ctx.cmd->Push(p);
            ctx.cmd->Dispatch2D({render_width_, render_height_});
          });
    }

    // Screen-space gi: add a diffuse bounce over the opaque result before
    // reflections (so reflections pick up the gi-lit color too). Raster tiers
    // only.
    if (ssgi_active && normals != kInvalidResource) {
      const f32 proj_scale[2] = {proj.m[0], proj.m[5]};
      ResourceHandle bounced = ssgi_.AddToGraph(
          graph_, scene_color, depth_export, normals, globals.inv_view_proj,
          proj_scale, 0.1f, frame_index_);
      scene_color = bounced;
      lit = bounced;
    }

    // Screen-space reflections over the opaque result (before transparency,
    // which does not reflect). Replaces scene_color downstream so everything
    // composites onto the reflected image. Only on raster tiers; rt tiers
    // reflect via the tlas.
    if (ssr_active && normals != kInvalidResource) {
      ResourceHandle reflected =
          ssr_.AddToGraph(graph_, scene_color, depth_export, normals, view_proj,
                          globals.inv_view_proj, view.camera.eye, frame_index_);
      scene_color = reflected;
      lit = reflected;
    }

    // The fluid surface draws inside the transparent pass, so a live fluid
    // domain must open it even when the scene submits no transparent meshes
    // (a lava-only or flood scene has nothing else to draw there).
    const bool fluid_draw =
        fluid_sim_active_ && fluid_sim_.active() && fluid_surface_ != nullptr;
    if ((!transparent.empty() || fluid_draw) && water_) {
      lit = add_water(scene_color, depth, depth_export, motion, sun_shadow,
                      shadow_atlas, csm_active, shadow_slot, tlas_slot,
                      /*globals_written=*/true, rcgi_irr);
    }

    // Surface weather: wet/darken (rain) and whiten (snow) the lit surfaces
    // before the atmosphere/clouds/rain layer over them, gated by the sky
    // occlusion map so cover stays dry. Wetness and snow cover are independent
    // channels (melting snow over wet ground coexists); the game integrates
    // them over time, and live precipitation acts as a floor so setting only
    // `precipitation` still wets (or whitens) the ground.
    const f32 live_rain =
        settings_.weather.snow ? 0.0f : settings_.weather.precipitation;
    const f32 live_snow =
        settings_.weather.snow ? settings_.weather.precipitation : 0.0f;
    const f32 surface_wetness = std::max(settings_.weather.wetness, live_rain);
    const f32 surface_snow = std::max(settings_.weather.snow_cover, live_snow);
    if ((surface_wetness > 0.0f || surface_snow > 0.0f) && !path_trace &&
        normals != kInvalidResource) {
      SurfaceWeather::Frame sf;
      sf.inv_view_proj = globals.inv_view_proj;
      sf.camera_pos = view.camera.eye;
      sf.wetness = surface_wetness;
      sf.snow_cover = surface_snow;
      sf.rain = live_rain;
      sf.time = static_cast<f32>(time_seconds_);
      if (precip_occlusion_active_) {
        precip_occlusion_.Params(sf.occl);
        sf.occl_range = PrecipOcclusion::y_range();
        sf.occlusion = precip_occlusion_.view();
        sf.occlusion_sampler = precip_occlusion_.sampler();
      }
      lit = surface_weather_.AddToGraph(
          graph_, lit, normals, depth_export, environment_->sky_view(),
          environment_->sampler(), {render_width_, render_height_}, sf);
    }

    // Aerial perspective: composite the atmosphere between the camera and each
    // surface so distant geometry hazes/blue-shifts like the sky. Cheap;
    // skipped when path tracing (the path tracer scatters its own sky).
    if (settings_.aerial_perspective > 0.0f && !path_trace && !interior) {
      AerialPerspective::Frame af;
      af.inv_view_proj = globals.inv_view_proj;
      af.camera_pos = view.camera.eye;
      af.sun_direction = settings_.sun_direction;
      af.sun_intensity = settings_.sun_intensity;
      af.sun_color = settings_.sun_color;
      af.strength = settings_.aerial_perspective;
      lit = aerial_perspective_.AddToGraph(graph_, lit, depth_export,
                                           environment_->transmittance_view(),
                                           environment_->multiscatter_view(),
                                           {render_width_, render_height_}, af);
    }

    // Volumetric clouds raymarched over the sky, composited against depth so
    // terrain occludes them. Skipped when path tracing. The opt-in cloudscape
    // model replaces the procedural pass outright; if its lazy init fails
    // (e.g. no 3D-image support) the legacy pass keeps the sky.
    bool cloudscape_on = settings_.cloudscape && cloudscape_ready_ && !path_trace && !interior;
    Cloudscape::Frame cloudscape_frame;
    bool cloudscape_haze_pending = false;
    if (cloudscape_on) {
      Cloudscape::Frame &cf = cloudscape_frame;
      cf.inv_view_proj = globals.inv_view_proj;
      cf.prev_view_proj = globals.prev_view_proj;
      cf.camera_pos = view.camera.eye;
      cf.jitter[0] = globals.jitter[0];
      cf.jitter[1] = globals.jitter[1];
      cf.reset_history = first_frame;
      cf.time = static_cast<f32>(time_seconds_);
      cf.frame_index = frame_index_;
      cf.sun_direction = settings_.sun_direction;
      // Lightning floods the deck from within via the full-res composite
      // (Frame::flash); the amortized march itself stays flash-free so the
      // temporal history never bakes a bright frame in. The damped global
      // level comes from the weather layer; the raw envelope drives the
      // directional glow a distant strike throws on its own horizon.
      cf.sun_intensity = settings_.sun_intensity;
      cf.sun_color = settings_.sun_color;
      cf.ambient = settings_.ambient;
      cf.flash = settings_.weather.lightning;
      if (settings_.weather.strike_age >= 0.0f) {
        cf.strike_active = true;
        cf.strike_pos = settings_.weather.strike_pos;
        cf.flash_raw = LightningSystem::Envelope(settings_.weather.strike_age,
                                                settings_.weather.strike_seed) *
                       settings_.weather.strike_energy;
      }
      cf.steps = settings_.cloudscape_steps;
      cf.transmittance_lut = environment_->transmittance_view();
      cf.lut_sampler = environment_->sampler();
      cf.controls = settings_.cloudscape_controls;
      // The weather struct owns the live wind; keep the deck advecting with it
      // even when the app writes controls without touching the wind fields.
      cf.controls.wind_yaw = settings_.weather.wind_yaw;
      cf.controls.wind_speed = settings_.weather.wind_speed;
      lit = cloudscape_.AddToGraph(graph_, lit, depth_export,
                                   {render_width_, render_height_}, cf);
      // Funnel before haze: the tornado hangs from the deck, then the ground
      // haze veils both.
      lit = cloudscape_.AddFunnelToGraph(graph_, lit, depth_export,
                                         {render_width_, render_height_}, cf);
      cloudscape_haze_pending = true;
    }
    if (!cloudscape_on && settings_.clouds && !path_trace && !interior) {
      Clouds::Frame cf;
      cf.inv_view_proj = globals.inv_view_proj;
      cf.camera_pos = view.camera.eye;
      cf.time = static_cast<f32>(time_seconds_);
      cf.sun_direction = settings_.sun_direction;
      // Lightning brightens the cloud (its ambient + sun terms scale with
      // intensity), so the storm clouds flash from within.
      cf.sun_intensity =
          settings_.sun_intensity + settings_.weather.lightning * 7.0f;
      cf.sun_color = settings_.sun_color;
      cf.coverage = settings_.cloud_coverage;
      cf.wind_x =
          std::cos(settings_.weather.wind_yaw) * settings_.weather.wind_speed;
      cf.wind_z =
          std::sin(settings_.weather.wind_yaw) * settings_.weather.wind_speed;
      lit = clouds_.AddToGraph(graph_, lit, depth_export,
                               {render_width_, render_height_}, cf);
    }

    // Lightning bolt: the procedural branched channel, drawn after the clouds
    // (the bolt overlays the deck) and before the froxel composite + precip
    // volume (fog scatters over it, rain streaks cross in front of it). Drawn
    // pre-resolve with a zero-motion core so TAA treats the flash as static.
    if (settings_.weather.strike_age >= 0.0f && lightning_.available() &&
        !path_trace && !interior && depth_export != kInvalidResource &&
        motion != kInvalidResource) {
      LightningSystem::Frame lf;
      lf.view_proj = view_proj;
      lf.cam_pos = view.camera.eye;
      lf.time = static_cast<f32>(time_seconds_);
      lf.strike_pos = settings_.weather.strike_pos;
      lf.strike_age = settings_.weather.strike_age;
      lf.strike_seed = settings_.weather.strike_seed;
      lf.strike_energy = settings_.weather.strike_energy;
      lf.jitter[0] = globals.jitter[0];
      lf.jitter[1] = globals.jitter[1];
      lightning_.AddToGraph(graph_, lit, depth_export, motion, lf);
    }

    if (cloudscape_haze_pending) {
      // Ground haze is the nearest medium, so it also veils the lightning
      // channel instead of letting the bolt draw crisply over thick murk.
      lit = cloudscape_.AddHazeToGraph(graph_, lit, depth_export,
                                       {render_width_, render_height_}, cloudscape_frame);
    }

    // Note: screen-space precipitation (rain/snow streaks) is composited after
    // the temporal/upscale resolve below, so TAA's history accumulation does
    // not smear the high-frequency streaks. Surface wetness (above) stays
    // pre-resolve since it shades real surfaces that should be anti-aliased.

    // Unified froxel volumetrics: near-field scattering from the sun and the
    // shadowed clustered lights, composited before the temporal pass so the
    // jittered volume resolves clean.
    bool froxel_on = settings_.froxel_fog && froxel_fog_.available() &&
                     !path_trace && depth_export != kInvalidResource;
    if (froxel_on) {
      FroxelFog::Frame ff;
      ff.inv_view_proj = globals.inv_view_proj;
      ff.prev_view_proj = globals.prev_view_proj;
      ff.camera_pos = view.camera.eye;
      ff.frame_index = frame_index_;
      ff.sun_direction = applied_sun_direction_;
      ff.anisotropy = settings_.fog_anisotropy;
      ff.sun_color = applied_sun_color_ * applied_sun_intensity_;
      ff.ambient = settings_.ambient * 0.05f;
      ff.density = settings_.froxel_density;
      ff.height_falloff = settings_.fog_height_falloff;
      ff.base_height = settings_.fog_base_height;
      std::memcpy(ff.cluster_params, globals.cluster_params,
                  sizeof(ff.cluster_params));
      ff.screen_size[0] = static_cast<f32>(render_width_);
      ff.screen_size[1] = static_cast<f32>(render_height_);
      ff.csm_active = csm_active;
      ff.lights = frame.lights;
      ff.cluster_counts = cluster_counts_;
      ff.cluster_indices = cluster_indices_;
      ff.local_shadow_faces = local_shadows_active_
                                  ? local_shadows_.face_buffer(frame_slot)
                                  : environment_->dummy_storage();
      ff.local_shadow_atlas = local_shadows_active_
                                  ? local_shadows_.atlas().view
                                  : environment_->shadow_dummy_view();
      ff.cascade_buffer =
          csm_active ? shadow_.cascade_buffer(shadow_slot) : GpuBuffer{};
      ff.cascade_size = shadow_.cascade_buffer_size();
      ff.comparison_sampler = environment_->comparison_sampler();
      froxel_fog_.AddToGraph(graph_, lit, depth_export,
                             csm_active ? shadow_atlas : kInvalidResource,
                             {render_width_, render_height_}, ff);
    }

    // Volumetric fog marches the lit scene against depth before the temporal
    // pass, so the marched noise resolves into stable shafts.
    if (fog_active) {
      VolumetricFog::Frame ff;
      ff.inv_view_proj = globals.inv_view_proj;
      ff.camera_pos = view.camera.eye;
      ff.sun_direction = settings_.sun_direction;
      ff.sun_intensity = settings_.sun_intensity;
      ff.sun_color = settings_.sun_color;
      ff.density = settings_.fog_density;
      ff.height_falloff = settings_.fog_height_falloff;
      ff.base_height = settings_.fog_base_height;
      ff.anisotropy = settings_.fog_anisotropy;
      ff.frame_index = frame_index_;
      lit = volumetric_fog_.AddToGraph(graph_, *raytracing_, tlas_slot, lit,
                                       depth_export,
                                       {render_width_, render_height_}, ff);
    }

    // Shell fur over the lit scene, depth-tested against the scene depth so the
    // core sphere occludes the far-side shells.
    if (view.fur_ball && !path_trace) {
      Mat4 model{};
      model.m[0] = model.m[5] = model.m[10] = model.m[15] = 1.0f;
      model.m[12] = view.fur_position.x;
      model.m[13] = view.fur_position.y;
      model.m[14] = view.fur_position.z;
      Vec3 sun_col = applied_sun_color_ * applied_sun_intensity_;
      fur_.AddToGraph(graph_, lit, depth, model, view_proj,
                      applied_sun_direction_, sun_col,
                      std::max(settings_.ambient, 0.12f), FurPass::Params{});
    }

    // Order-independent transparency (weighted blended) over the lit scene.
    if (!view.oit.empty() && !path_trace) {
      Vec3 sun_col = applied_sun_color_ * applied_sun_intensity_;
      WboitPass::LightingContext oit_light;
      oit_light.lights = frame.lights;
      oit_light.cluster_counts = cluster_counts_;
      oit_light.cluster_indices = cluster_indices_;
      std::memcpy(oit_light.cluster_params, globals.cluster_params,
                  sizeof(oit_light.cluster_params));
      oit_light.froxel_volume = froxel_fog_.integrated().view;
      oit_light.froxel_sampler = froxel_fog_.volume_sampler();
      oit_light.froxel_near = FroxelFog::kNear;
      oit_light.froxel_far = FroxelFog::kFar;
      oit_light.froxel_enabled = froxel_on;
      lit = wboit_.AddToGraph(graph_, lit, depth, view.oit, view_proj,
                              applied_sun_direction_, sun_col,
                              std::max(settings_.ambient, 0.12f), render_width_,
                              render_height_, oit_light);
    }

    // True volumetric precipitation: stateless world-anchored rain streaks /
    // snow flakes with impact splashes, drawn pre-resolve with motion vectors
    // so TAA treats them like geometry. Gated by the sky-occlusion map (nothing
    // falls under the shelter roof). When this runs, the post-resolve
    // screen-space streak layer is skipped; it remains the volumetric=false and
    // path-trace fallback.
    if (precip_occlusion_active_ && precip_volume_ready &&
        depth_export != kInvalidResource && motion != kInvalidResource) {
      Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
      Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
      PrecipVolume::Frame vf;
      vf.view_proj = view_proj;
      vf.prev_view_proj = globals.prev_view_proj;
      vf.cam_right = right;
      vf.cam_up = Cross(right, fwd);
      vf.cam_pos = view.camera.eye;
      vf.sun_direction = applied_sun_direction_;
      vf.sun_color = applied_sun_color_;
      vf.sun_intensity = applied_sun_intensity_;
      vf.ambient = std::max(settings_.ambient, 0.02f);
      vf.time = static_cast<f32>(time_seconds_);
      vf.dt = view.frame_delta_seconds;
      vf.intensity = settings_.weather.precipitation;
      vf.snow = settings_.weather.snow;
      // Wind yaw is the direction the wind blows toward; decompose to xz.
      vf.wind[0] =
          std::cos(settings_.weather.wind_yaw) * settings_.weather.wind_speed;
      vf.wind[1] =
          std::sin(settings_.weather.wind_yaw) * settings_.weather.wind_speed;
      vf.gustiness = settings_.weather.gustiness;
      vf.lightning = settings_.weather.lightning;
      vf.jitter[0] = globals.jitter[0];
      vf.jitter[1] = globals.jitter[1];
      precip_occlusion_.Params(vf.occl);
      vf.occl_range = PrecipOcclusion::y_range();
      vf.occlusion = precip_occlusion_.view();
      vf.occlusion_sampler = precip_occlusion_.sampler();
      vf.froxel_enabled = froxel_on;
      vf.froxel_volume = froxel_fog_.integrated().view;
      vf.froxel_sampler = froxel_fog_.volume_sampler();
      vf.rt_shadows = precip_rt; // matched to the TLAS build request above
      precip_volume_.AddToGraph(graph_, lit, depth_export, motion,
                                raytracing_.get(), tlas_slot, vf);
      precip_volume_drawn = true;
    }

    // NIF particle emitters: a cpu pool per placed instance of an emitting
    // mesh, stepped here and appended to the frame's billboard sets (lit
    // smoke/mist plus HDR additive fire).
    base::Vector<ParticleInstance> emitter_lit;
    base::Vector<ParticleInstance> emitter_additive;
    if (!mesh_emitters_.empty()) {
      emitter_sim_.BeginFrame(view.frame_delta_seconds, view.camera.eye);
      for (const DrawItem &item : view.draws) {
        if (const auto *emitters = mesh_emitters_.find(item.mesh)) {
          emitter_sim_.AddInstance(item.mesh, *emitters, item.transform);
        }
      }
      emitter_sim_.Simulate(&emitter_lit, &emitter_additive);
    }

    // Lit billboard particles blend over the resolved scene, faded against the
    // prepass depth, before temporal reconstruction. Either a cpu-uploaded set
    // or the gpu-simulated fountain.
    if (!view.particles.empty() || !emitter_lit.empty() ||
        !emitter_additive.empty() || view.gpu_particle_count > 0) {
      Vec3 fwd = Normalize(view.camera.target - view.camera.eye);
      Vec3 right = Normalize(Cross(fwd, Vec3{0, 1, 0}));
      ParticleSystem::Frame pf;
      pf.view_proj = view_proj;
      pf.prev_view_proj = globals.prev_view_proj;
      pf.cam_right = right;
      pf.cam_up = Cross(right, fwd);
      pf.sun_direction = settings_.sun_direction;
      pf.sun_color = settings_.sun_color;
      pf.sun_intensity = settings_.sun_intensity;
      pf.ambient = std::max(settings_.ambient, 0.15f);
      pf.near_plane = 0.1f;
      pf.soft_fade = 0.6f;
      pf.jitter[0] = globals.jitter[0];
      pf.jitter[1] = globals.jitter[1];
      // Lit translucency inputs: clustered lights + shadows + the fog volume.
      std::memcpy(pf.cluster_params, globals.cluster_params,
                  sizeof(pf.cluster_params));
      pf.froxel_near = FroxelFog::kNear;
      pf.froxel_far = FroxelFog::kFar;
      pf.froxel_enabled = froxel_on;
      pf.lights = frame.lights;
      pf.cluster_counts = cluster_counts_;
      pf.cluster_indices = cluster_indices_;
      pf.local_shadow_faces = local_shadows_active_
                                  ? local_shadows_.face_buffer(frame_slot)
                                  : environment_->dummy_storage();
      pf.local_shadow_atlas = local_shadows_active_
                                  ? local_shadows_.atlas().view
                                  : environment_->shadow_dummy_view();
      pf.comparison_sampler = environment_->comparison_sampler();
      pf.froxel_volume = froxel_fog_.integrated().view;
      pf.froxel_sampler = froxel_fog_.volume_sampler();
      if (view.gpu_particle_count > 0) {
        ParticleSystem::Sim sim;
        sim.emitter[0] = view.gpu_particle_emitter.x;
        sim.emitter[1] = view.gpu_particle_emitter.y;
        sim.emitter[2] = view.gpu_particle_emitter.z;
        sim.dt = view.frame_delta_seconds;
        sim.count = view.gpu_particle_count;
        sim.mode = view.gpu_particle_mode;
        sim.radius = view.gpu_particle_radius;
        sim.intensity = view.gpu_particle_intensity;
        sim.time = static_cast<f32>(time_seconds_);
        pf.emissive = view.gpu_particle_mode == 1;
        BindingSetHandle particle_bindless =
            bindless_ ? bindless_->set() : BindingSetHandle{};
        particles_.SimulateAndDraw(graph_, lit, depth_export, motion, sim, pf,
                                   frame_index_ % 2, particle_bindless);
      } else {
        base::Vector<ParticleInstance> &demo_particles =
            view.particles_emissive ? emitter_additive : emitter_lit;
        for (const ParticleInstance &inst : view.particles)
          demo_particles.push_back(inst);
        BindingSetHandle particle_bindless =
            bindless_ ? bindless_->set() : BindingSetHandle{};
        particles_.AddToGraph(graph_, lit, depth_export, motion, emitter_lit,
                              emitter_additive, pf, frame_index_ % 2,
                              particle_bindless);
      }
    }

    // 3D gaussian splats: non-triangle primitives blended over the resolved
    // scene.
    if (!view.gaussians.empty()) {
      GaussianSplat::Frame gf;
      gf.view = view_mat;
      gf.proj_x = proj.m[0];
      gf.proj_y = proj.m[5];
      gf.near_plane = 0.1f;
      gf.screen_x = static_cast<f32>(render_width_);
      gf.screen_y = static_cast<f32>(render_height_);
      gaussians_.AddToGraph(graph_, lit, view.gaussians, gf, frame_index_ % 2);
    }

    // Bounds / acceleration-structure debug view: overlay the cull bounding
    // boxes. Mesh-shader meshlet demo: clusters cull + draw on the gpu,
    // composited into the lit scene with depth. Only active when a meshlet mesh
    // was uploaded.
    if (meshlet_.active()) {
      meshlet_visible_ =
          meshlet_.last_visible(frame_index_); // fence-safe, read before reset
      f32 planes[5][4];
      ExtractFrustumPlanes(view_proj, planes);
      meshlet_.AddToGraph(graph_, lit, depth, view_proj, planes,
                          view.camera.eye, frame_index_);
    }
    // Distant foliage imposters: instanced octahedral billboards with depth.
    const char *imposters_env = std::getenv("RX_IMPOSTERS");
    if (imposters_.active() && (!imposters_env || imposters_env[0] != '0')) {
      ImposterPass::Frame imf;
      imf.view_proj = view_proj;
      imf.camera_pos = view.camera.eye;
      imf.sun_direction = applied_sun_direction_;
      imf.sun_intensity = applied_sun_intensity_;
      imf.sun_color = applied_sun_color_;
      imposters_.AddToGraph(graph_, lit, depth, {render_width_, render_height_},
                            imf);
    }

    // Strand hair: ribbon draw over the lit scene with depth, node positions
    // fed by the physics strand sim through SetHairGroomPoints.
    if (hair_.active()) {
      HairStrands::Frame hf;
      hf.view_proj = view_proj;
      hf.camera_pos = view.camera.eye;
      hf.sun_direction = applied_sun_direction_;
      hf.sun_intensity = applied_sun_intensity_;
      hf.sun_color = applied_sun_color_;
      hair_.AddToGraph(graph_, lit, depth, {render_width_, render_height_}, hf,
                       frame_slot);
    }

    // Virtual geometry: cluster-DAG LOD cut, two-pass occlusion cull and
    // visibility-buffer raster, all on the gpu (single-pass fallback inside).
    if (vgeo_.active()) {
      VirtualGeometryPass::Frame vf;
      vf.view_proj = view_proj;
      ExtractFrustumPlanes(view_proj, vf.planes);
      vf.eye = view.camera.eye;
      // Screen pixels per world unit at distance 1 (|proj.m5| carries the
      // vulkan y-flip, hence the fabs).
      vf.proj_scale =
          std::fabs(proj.m[5]) * static_cast<f32>(render_height_) * 0.5f;
      vf.proj_m00 = std::fabs(proj.m[0]);
      vf.proj_m11 = std::fabs(proj.m[5]);
      vf.error_pixels = VgeoError.get() > 0.0f ? VgeoError.get() : 1.0f;
      // Reversed-z infinite far: proj m[14] is the near plane distance.
      vf.near_plane = proj.m[14] > 0.0f ? proj.m[14] : 0.1f;
      vf.color = lit;
      vf.depth = depth;
      vf.width = render_width_;
      vf.height = render_height_;
      vf.debug = static_cast<u32>(std::max(VgeoDebug.get(), 0));
      vf.slot = frame_index_;
      vgeo_.AddToGraph(*device_, graph_, vf);
    }

    if (settings_.debug_view == DebugView::kBounds) {
      gpu_cull_.AddBoundsPass(graph_, lit, view_proj, cull_instance_count,
                              cull_slot);
    }

    // App translucents over the fully composited scene (after rx transparency,
    // before post/tonemap). Blends into `lit`, depth-tested against rx
    // geometry. Motion is passed here too: this phase still precedes TAA,
    // upscaling, and motion blur, and rx's own transparent particles already
    // alpha-blend velocity into `motion`, so app translucents get the same
    // target to overwrite it on moving surfaces (no depth_export in this phase).
    if (view.scene_transparent && !path_trace && depth != kInvalidResource) {
      add_scene_hook(view.scene_transparent, ScenePhase::kTransparent, lit,
                     depth, kInvalidResource, motion);
    }

    // Overdraw debug view: clear lit and additive-replay all geometry so the
    // heat ramp shows how many layers each pixel shaded.
    if (settings_.debug_view == DebugView::kOverdraw) {
      graph_.AddPass(
          "overdraw",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Write(lit, ResourceUsage::kColorAttachment);
          },
          [this, lit, view_proj, &view](PassContext &ctx) {
            overdraw_.Render(
                *ctx.cmd, ctx.graph->image(lit).view,
                {render_width_, render_height_}, view_proj,
                [this, &view, view_proj](CommandList &cmd) {
                  for (const DrawItem &item : view.draws) {
                    const GpuMesh *mesh = meshes_.find(item.mesh);
                    if (!mesh || !mesh->indices)
                      continue;
                    // view_proj sits at offset 0 (pushed by Render); the model
                    // follows it per draw.
                    cmd.PushConstants(&item.transform, sizeof(Mat4),
                                      sizeof(Mat4));
                    cmd.BindVertexBuffer(0, mesh->vertices);
                    cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
                    for (const GpuSubmesh &submesh : mesh->submeshes) {
                      cmd.DrawIndexed(submesh.index_count, 1,
                                      submesh.index_offset, 0, 0);
                    }
                  }
                  overdraw_.BindInstanced(cmd, view_proj);
                  for (const InstanceStore::Group &group :
                       instances_.groups()) {
                    if (!group.alive)
                      continue;
                    const GpuMesh *mesh = meshes_.find(group.mesh);
                    if (!mesh || !mesh->indices)
                      continue;
                    cmd.BindVertexBuffer(0, mesh->vertices);
                    cmd.BindVertexBuffer(1, group.buffer);
                    cmd.BindIndexBuffer(mesh->indices, 0, IndexType::kUint32);
                    for (const GpuSubmesh &submesh : mesh->submeshes) {
                      cmd.DrawIndexed(submesh.index_count,
                                      static_cast<u32>(group.transforms.size()),
                                      submesh.index_offset, 0, 0);
                    }
                  }
                });
          });
    }

    // Editor debug lines over the resolved scene (depth-tested + overlay), just
    // before post/UI. Only added when the app supplied lines this frame.
    if (!view.debug_lines.empty() || !view.debug_lines_overlay.empty()) {
      graph_.AddPass(
          "debug_lines",
          [lit, depth](RenderGraph::PassBuilder &builder) {
            builder.Write(lit, ResourceUsage::kColorAttachment);
            if (depth != kInvalidResource)
              builder.Write(depth, ResourceUsage::kDepthAttachment);
          },
          [this, lit, depth, view_proj, &view](PassContext &ctx) {
            const GpuImage &color = ctx.graph->image(lit);
            ColorAttachment ca{.view = color.view, .load = LoadOp::kLoad};
            const bool have_depth = depth != kInvalidResource;
            DepthAttachment da{};
            if (have_depth)
              da = {.view = ctx.graph->image(depth).view,
                    .load = LoadOp::kLoad};
            ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                     .colors = {&ca, 1},
                                     .depth = have_depth ? &da : nullptr});
            DrawDebugLines(*ctx.cmd, view, view_proj,
                           {render_width_, render_height_});
            ctx.cmd->EndRendering();
          });
    }

    // SDF clipmap debug raymarch (RX_SDF_DEBUG): replace the lit scene with a
    // view built purely from the SDF clipmap (1 = distance field tinted by
    // clip, 2 = hit albedo + gradient-normal shading). Standalone S1
    // verification.
    if (sdf_clipmap_ && sdf_available_ && SdfDebugOpt) {
      sdf_clipmap_->AddDebugPass(
          graph_, lit, {render_width_, render_height_}, globals.inv_view_proj,
          view.camera.eye, static_cast<u32>(static_cast<int>(SdfDebugOpt)),
          frame_index_);
    }
  } // end raster path

  // Water has no place in the path tracer (blend geometry never enters the
  // tlas), so composite the raster water pass over the path-traced image. A
  // small opaque depth prepass (direct draws, no gpu cull) gives water correct
  // occlusion and soft shorelines; reflections/shadows trace inline against the
  // path tracer's tlas. The atmosphere/cloud passes that normally precede water
  // are skipped under path tracing, so water is simply the last thing
  // composited.
  if (path_trace && water_pipeline_active && !transparent.empty()) {
    ResourceHandle pt_normals =
        graph_.CreateTexture({.name = "pt_water_normals",
                              .format = kNormalFormat,
                              .width = render_width_,
                              .height = render_height_});
    ResourceHandle pt_motion = graph_.CreateTexture({.name = "pt_water_motion",
                                                     .format = kMotionFormat,
                                                     .width = render_width_,
                                                     .height = render_height_});
    ResourceHandle pt_depth = graph_.CreateTexture({.name = "pt_water_depth",
                                                    .format = kDepthFormat,
                                                    .width = render_width_,
                                                    .height = render_height_});
    ResourceHandle pt_depth_export =
        graph_.CreateTexture({.name = "pt_water_depth_export",
                              .format = Format::kR32Float,
                              .width = render_width_,
                              .height = render_height_});
    graph_.AddPass(
        "pt_water_prepass",
        [&](RenderGraph::PassBuilder &builder) {
          builder.Write(pt_normals, ResourceUsage::kColorAttachment);
          builder.Write(pt_motion, ResourceUsage::kColorAttachment);
          builder.Write(pt_depth_export, ResourceUsage::kColorAttachment);
          builder.Write(pt_depth, ResourceUsage::kDepthAttachment);
        },
        [this, pt_normals, pt_motion, pt_depth_export, pt_depth, globals_set,
         update_globals_set, frame_slot, &frame, &view,
         view_proj](PassContext &ctx) {
          // First globals-set user on the path-traced frame: uniform + tlas
          // (the transparent pass right after wants the tlas for water
          // reflections).
          update_globals_set(ctx, kInvalidResource, false, /*want_tlas=*/true);

          ColorAttachment colors[3];
          colors[0] = {.view = ctx.graph->image(pt_normals).view};
          colors[1] = {.view = ctx.graph->image(pt_motion).view};
          colors[2] = {.view = ctx.graph->image(pt_depth_export).view};
          DepthAttachment depth_attachment{
              .view = ctx.graph->image(pt_depth).view,
              .clear = 0.0f}; // reversed z clears to far = 0
          ctx.cmd->BeginRendering({.extent = {render_width_, render_height_},
                                   .colors = {colors, 3},
                                   .depth = &depth_attachment});

          environment_->WriteEnvSet(
              env_prepass_sets_[frame_slot], TextureView{}, nullptr,
              TextureView{}, GpuBuffer{}, 0, TextureView{}, TextureView{},
              GpuBuffer{}, 0, TextureView{}, GpuBuffer{}, GpuBuffer{},
              GpuBuffer{}, GpuBuffer{}, TextureView{}, GpuBuffer{},
              TextureView{}, TextureView{}, TextureView{}, TextureView{},
              GpuBuffer{}, TextureView{}, TextureView{},
              fft_ocean_active_ ? ocean_.displacement_view() : TextureView{},
              fft_ocean_active_ ? ocean_.normal_foam_view() : TextureView{});
          mesh_pipeline_->BindPrepass(*ctx.cmd, globals_set,
                                      env_prepass_sets_[frame_slot]);
          BindingSetHandle bound_material{};
          bool skinned_bound = false;
          bool masked_bound =
              false; // BindPrepass bound the opaque static variant
          for (const DrawItem &item : view.draws) {
            const GpuMesh *mesh = meshes_.find(item.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            bool draw_skinned = mesh->skinned && mesh_pipeline_->has_skinning();
            MeshPushConstants push{.model = item.transform,
                                   .prev_model = item.prev_transform};
            if (mesh->terrain_lod) {
              std::memcpy(push.detail_rect, view.detail_rect,
                          sizeof(push.detail_rect));
            }
            if (draw_skinned && item.skin_offset >= 0) {
              push.bone_address = frame.bone_palette.address;
              push.skin_offset = static_cast<u32>(item.skin_offset);
            }
            if (mesh->morph_target_count > 0 && item.morph_offset >= 0 &&
                item.morph_count > 0) {
              push.morph_delta_address = mesh->morph_deltas.address;
              push.morph_weight_address = frame.morph_weights.address;
              push.morph_first = static_cast<u32>(item.morph_offset);
              push.morph_count = item.morph_count;
              push.morph_vertex_count = mesh->vertex_count;
            }
            mesh_pipeline_->Draw(*ctx.cmd, *mesh, push);
            for (const GpuSubmesh &submesh : mesh->submeshes) {
              if (submesh.blend)
                continue; // transparency owns its own depth
              if (draw_skinned != skinned_bound ||
                  submesh.alpha_mask != masked_bound) {
                mesh_pipeline_->SetPrepassVariant(*ctx.cmd, draw_skinned,
                                                  submesh.alpha_mask);
                skinned_bound = draw_skinned;
                masked_bound = submesh.alpha_mask;
              }
              BindingSetHandle material =
                  material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              mesh_pipeline_->DrawSubmesh(*ctx.cmd, submesh);
            }
          }
          f32 instance_planes[5][4];
          ExtractFrustumPlanes(view_proj, instance_planes);
          for (const InstanceStore::Group &group : instances_.groups()) {
            if (!group.alive ||
                (group.cullable &&
                 SphereOutsideFrustum(instance_planes, group.bounds_center,
                                      group.bounds_radius))) {
              continue;
            }
            const GpuMesh *mesh = meshes_.find(group.mesh);
            if (!mesh || mesh->all_blend)
              continue;
            MeshPushConstants push{};
            const GpuBuffer &previous =
                group.previous_buffer ? group.previous_buffer : group.buffer;
            mesh_pipeline_->DrawInstances(*ctx.cmd, *mesh, group.buffer,
                                          previous, push);
            for (const GpuSubmesh &submesh : mesh->submeshes) {
              if (submesh.blend)
                continue;
              mesh_pipeline_->SetInstancedPrepass(*ctx.cmd, submesh.alpha_mask);
              const BindingSetHandle material =
                  material_system_->set(submesh.material);
              if (!(material == bound_material)) {
                mesh_pipeline_->BindMaterial(*ctx.cmd, material);
                bound_material = material;
              }
              ctx.cmd->DrawIndexed(submesh.index_count,
                                   static_cast<u32>(group.transforms.size()),
                                   submesh.index_offset, 0, 0);
            }
          }
          ctx.cmd->EndRendering();
        });
    lit = add_water(scene_color, pt_depth, pt_depth_export, pt_motion,
                    kInvalidResource, kInvalidResource, false, 0u, tlas_slot,
                    /*globals_written=*/true);
  }

  // Rebuild next frame's shading rates from the finished render-res frame
  // (post-transparency, pre-resolve: what the scene pass actually shades).
  if (vrs_active_) {
    vrs_.AddToGraph(graph_, lit, motion, {render_width_, render_height_},
                    settings_.vrs_threshold, /*motion_scale=*/2.5f);
  }

  // RCGI screen cache: snapshot this frame's lit HDR colour + depth so next
  // frame's final gather can read last-frame radiance for its screen cache.
  // Only when the M2 gather ran (probes-only / off record nothing).
  if (rcgi_world && !rcgi_probes_only && lit != kInvalidResource &&
      depth_export != kInvalidResource) {
    rcgi_->AddHistoryCopy(graph_, lit, depth_export,
                          {render_width_, render_height_});
  }

  // The path tracer already resolved antialiasing through accumulation; the
  // raster path runs its temporal/upscale resolve here.
  ResourceHandle post_input = lit;
  if (!path_trace) {
    switch (settings_.aa_mode) {
    case AntiAliasingMode::kTaa:
      post_input = taa_.AddToGraph(
          graph_, lit, motion, frame_index_,
          settings_.debug_view == DebugView::kTemporalHistory ? 1u
          : settings_.debug_view == DebugView::kMotionVectors ? 2u
                                                              : 0u);
      break;
    case AntiAliasingMode::kUpscaler: {
      ResourceHandle upscaled = upscaler_->AddToGraph(
          graph_, {.color = lit,
                   .depth = depth_export,
                   .motion_vectors = motion,
                   .jitter_x = jitter_x,
                   .jitter_y = jitter_y,
                   .sharpness = settings_.sharpness,
                   .frame_delta_seconds = view.frame_delta_seconds,
                   .camera_near = 0.1f,
                   .camera_fov_y = view.camera.fov_y,
                   .reset_history = first_frame});
      if (upscaled != kInvalidResource)
        post_input = upscaled;
      break;
    }
    case AntiAliasingMode::kNone:
      break;
    }
  }

  // Dimensions of the aa-resolved image the post stack runs at.
  bool upscaled = !path_trace &&
                  settings_.aa_mode == AntiAliasingMode::kUpscaler &&
                  post_input != lit;
  u32 post_width = upscaled ? output_width_ : render_width_;
  u32 post_height = upscaled ? output_height_ : render_height_;

  // Depth of field on the resolved frame, before motion blur streaks over it.
  if (settings_.dof && !path_trace && depth_export != kInvalidResource) {
    DepthOfFieldPass::Frame df;
    df.aperture = settings_.dof_aperture;
    df.focus_distance = settings_.dof_focus;
    post_input = dof_.AddToGraph(graph_, post_input, depth_export,
                                 {post_width, post_height}, df);
  }

  // Motion blur right after the AA resolve (before precipitation streaks and
  // the linear-hdr export). Uses the render-res prepass velocity; uv-space
  // velocities are resolution independent so the upscaled path works too.
  if (settings_.motion_blur && !path_trace && motion != kInvalidResource) {
    MotionBlurPass::Frame mb;
    mb.shutter = settings_.motion_blur_shutter;
    mb.frame_index = frame_index_;
    if (MotionBlurDebugVel.overridden()) {
      mb.debug_velocity[0] = static_cast<f32>(double(MotionBlurDebugVel));
    }
    post_input = motion_blur_.AddToGraph(graph_, post_input, motion,
                                         {post_width, post_height}, mb);
  }

  // Screen-space precipitation streaks, composited at output resolution after
  // the AA resolve so they stay crisp (TAA would otherwise smear them) and
  // tonemap with the scene. Driven by weather; surface wetness was applied
  // pre-resolve. The non-volumetric fallback: skipped whenever the 3D particle
  // volume drew.
  if (settings_.weather.precipitation > 0.0f && !path_trace &&
      !precip_volume_drawn) {
    Precipitation::Frame pf;
    pf.inv_view_proj = globals.inv_view_proj;
    pf.camera_pos = view.camera.eye;
    pf.time = static_cast<f32>(time_seconds_);
    pf.intensity = settings_.weather.precipitation;
    pf.snow = settings_.weather.snow;
    post_input = precipitation_.AddToGraph(graph_, post_input,
                                           {post_width, post_height}, pf);
  }

  // Linear-hdr export: copy the resolved scene (pre-tonemap) into a host
  // buffer.
  hdr_pending_ = false;
  if (!hdr_path_.empty() && time_seconds_ >= hdr_at_) {
    u64 need = static_cast<u64>(post_width) * post_height * sizeof(f32) * 4;
    if (hdr_readback_.size != need) {
      device_->DestroyBuffer(hdr_readback_);
      hdr_readback_ = device_->CreateBuffer(need, kBufferUsageStorage, true);
    }
    hdr_width_ = post_width;
    hdr_height_ = post_height;
    hdr_pending_ = hdr_readback_.mapped != nullptr;
    if (hdr_pending_) {
      graph_.AddPass(
          "hdr_capture",
          [&](RenderGraph::PassBuilder &builder) {
            builder.Read(post_input, ResourceUsage::kSampledCompute);
          },
          [this, post_input, post_width, post_height](PassContext &ctx) {
            u32 push[2] = {post_width, post_height};
            ctx.cmd->BindPipeline(hdr_pipeline_);
            ctx.cmd->BindTransient(
                0, {Bind::StorageBuffer(0, hdr_readback_),
                    Bind::Sampled(1, ctx.graph->image(post_input))});
            ctx.cmd->PushConstants(push, sizeof(push));
            ctx.cmd->Dispatch2D({post_width, post_height});
            ctx.cmd->MemoryBarrier(BarrierScope::kComputeWrite,
                                   BarrierScope::kHostRead);
          });
    }
  }

  exposure_.AddToGraph(graph_, post_input, post_width, post_height,
                       view.frame_delta_seconds);
  ResourceHandle bloom = kInvalidResource;
  ResourceHandle flare_src = kInvalidResource;
  if (settings_.bloom) {
    bloom =
        bloom_.AddToGraph(graph_, post_input, post_width, post_height,
                          settings_.lens_flare > 0.0f ? &flare_src : nullptr);
  }

  ResourceHandle backbuffer =
      graph_.ImportBackbuffer(swapchain_->image(image_index));

  post_->SetGrade(
      settings_.color_grade); // rebakes the lut only when it changes
  PostPass::Params post_params{
      static_cast<u32>(settings_.tonemap), settings_.bloom_intensity,
      bloom != kInvalidResource ? 1u : 0u,
      settings_.color_grade != ColorGrade::kNeutral ? 1u : 0u};
  switch (swapchain_->color_space()) {
  case ColorSpace::kHdr10Pq:
    post_params.output_transfer = 1;
    break;
  case ColorSpace::kScRgbLinear:
    post_params.output_transfer = 2;
    break;
  default:
    break;
  }
  if (int forced = HdrForceTransfer; forced == 1 || forced == 2) {
    post_params.output_transfer = static_cast<u32>(forced);
  }
  post_params.paper_white = settings_.hdr_paper_white;
  // Flare needs its prefiltered highlight source from the bloom chain; without
  // bloom the ghosts would mirror the raw scene.
  post_params.flare_intensity =
      flare_src != kInvalidResource ? settings_.lens_flare : 0.0f;
  post_params.aberration = settings_.chromatic_aberration;
  post_params.vignette = settings_.vignette;
  post_params.grain = settings_.film_grain;
  post_params.grain_seed = static_cast<f32>(frame_index_ % 1024) * 0.6180339f;
  graph_.AddPass(
      "post",
      [&](RenderGraph::PassBuilder &builder) {
        builder.Read(post_input, ResourceUsage::kSampledFragment);
        if (bloom != kInvalidResource)
          builder.Read(bloom, ResourceUsage::kSampledFragment);
        if (flare_src != kInvalidResource)
          builder.Read(flare_src, ResourceUsage::kSampledFragment);
        builder.Write(backbuffer, ResourceUsage::kColorAttachment);
      },
      [this, post_input, bloom, flare_src, backbuffer,
       post_params](PassContext &ctx) {
        TextureView bloom_view = bloom != kInvalidResource
                                     ? ctx.graph->image(bloom).view
                                     : ctx.graph->image(post_input).view;
        TextureView flare_view = flare_src != kInvalidResource
                                     ? ctx.graph->image(flare_src).view
                                     : bloom_view;
        post_->Record(ctx, ctx.graph->image(post_input).view, bloom_view,
                      flare_view, exposure_.exposure_buffer(),
                      exposure_.exposure_buffer_size(),
                      ctx.graph->image(backbuffer).view,
                      ctx.graph->image(backbuffer).extent, post_params);
      });

#if defined(RX_HAS_FSR3)
  // Frame generation: snapshot the pre-UI backbuffer as the interpolation
  // source, so the generated frame carries no smeared UI (it is re-drawn
  // crisp onto the interpolated image in the present path).
  if (fg_active_frame_ && framegen_) {
    graph_.AddPass(
        "fg_hudless",
        [&](RenderGraph::PassBuilder &b) {
          b.Write(backbuffer, ResourceUsage::kColorAttachment);
        },
        [this, backbuffer](PassContext &ctx) {
          const GpuImage &src = ctx.graph->image(backbuffer);
          const GpuImage &dst = framegen_->hudless();
          TextureBarrier pre[] = {Transition(src, ResourceState::kColorTarget,
                                             ResourceState::kCopySrc),
                                  Transition(dst,
                                             ResourceState::kShaderReadCompute,
                                             ResourceState::kCopyDst)};
          ctx.cmd->TextureBarriers(pre);
          ctx.cmd->CopyTexture(src, dst);
          TextureBarrier post[] = {
              Transition(src, ResourceState::kCopySrc,
                         ResourceState::kColorTarget),
              Transition(dst, ResourceState::kCopyDst,
                         ResourceState::kShaderReadCompute)};
          ctx.cmd->TextureBarriers(post);
        });
  }
#endif

  if (view.ui_draw || view.hud_draw) {
    // Backdrop blur: when a frosted widget is present (and the surface lets us
    // sample the backbuffer), capture + Gaussian-blur the post-tonemap frame
    // into ui_frost; the UI backend samples it for frosted panels.
    ResourceHandle ui_frost = kInvalidResource;
    if (view.hud_draw && view.needs_blur && ui_blur_ &&
        swapchain_->can_sample()) {
      ui_frost = ui_blur_->AddToGraph(graph_, backbuffer, output_width_,
                                      output_height_);
    }

    graph_.AddPass(
        "ui",
        [&](RenderGraph::PassBuilder &builder) {
          if (ui_frost != kInvalidResource)
            builder.Read(ui_frost, ResourceUsage::kSampledFragment);
          builder.Write(backbuffer, ResourceUsage::kColorAttachment);
        },
        [this, backbuffer, ui_frost, &view](PassContext &ctx) {
          ColorAttachment color{.view = ctx.graph->image(backbuffer).view,
                                .load = LoadOp::kLoad};
          // Hand the blurred backdrop to the UI before it records (the closure
          // reads view.blur_source); null when blur is not in play this frame.
          view.blur_source = ui_frost != kInvalidResource
                                 ? ctx.graph->image(ui_frost).view
                                 : TextureView{};
          view.blur_sampler = ui_frost != kInvalidResource ? ui_blur_->sampler()
                                                           : SamplerHandle{};
          ctx.cmd->BeginRendering(
              {.extent = ctx.graph->image(backbuffer).extent,
               .colors = {&color, 1}});
          if (view.hud_draw)
            view.hud_draw(*ctx.cmd);
          if (view.ui_draw)
            view.ui_draw(*ctx.cmd);
          ctx.cmd->EndRendering();
        });
  }
}

bool Renderer::CreateFrameResources() {
  for (FrameResources &frame : frames_) {
    frame.globals =
        device_->CreateBuffer(sizeof(FrameGlobals), kBufferUsageUniform, true);
    if (!frame.globals.mapped)
      return false;

    // Bone palette: host visible, read in the skinned vertex shader through its
    // device address (no descriptor binding). Column-major 4x4 per bone.
    frame.bone_palette = device_->CreateBuffer(
        static_cast<u64>(kMaxFrameBones) * sizeof(Mat4),
        kBufferUsageStorage | kBufferUsageDeviceAddress, true);
    if (!frame.bone_palette.mapped)
      return false;

    // Morph weights: host visible (target, weight) pairs, read like the bones.
    frame.morph_weights = device_->CreateBuffer(
        static_cast<u64>(kMaxFrameMorphWeights) * sizeof(MorphWeight),
        kBufferUsageStorage | kBufferUsageDeviceAddress, true);
    if (!frame.morph_weights.mapped)
      return false;

    frame.lights = device_->CreateBuffer(static_cast<u64>(kMaxFrameLights) *
                                             sizeof(PointLight),
                                         kBufferUsageStorage, true);
    if (!frame.lights.mapped)
      return false;
    frame.decals =
        device_->CreateBuffer(static_cast<u64>(kMaxFrameDecals) * sizeof(Decal),
                              kBufferUsageStorage, true);
    if (!frame.decals.mapped)
      return false;
  }
  return true;
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
  u32 width = window_->width();
  u32 height = window_->height();
  if (width == 0 || height == 0)
    return; // minimized
  device_->WaitIdle();
  swapchain_.reset();
  swapchain_hdr_request_ = WantHdrSwapchain();
  swapchain_ = device_->CreateSwapchain(width, height, settings_.vsync,
                                        swapchain_hdr_request_);
  if (!swapchain_)
    return;
  output_width_ = swapchain_->extent().width;
  output_height_ = swapchain_->extent().height;

  // The upscaler is sized for the output, rebuild it alongside.
  if (upscaler_) {
    upscaler_.reset();
    if (!CreateUpscalerForSettings()) {
      settings_.upscaler = UpscalerKind::kNone;
      settings_.aa_mode = AntiAliasingMode::kTaa;
      applied_upscaler_ = UpscalerKind::kNone;
    }
  }
  // The frame generator is sized for the swapchain; lazily recreated.
  framegen_.reset();
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
  swapchain_.reset();
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
      for (GpuMesh::LodRt &rt : kv.value.lod_rt)
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
    sdf_clipmap_.reset();
    sdf_scene_.reset();
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
    for (GpuBuffer &vbo : debug_line_vbo_)
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
    vrs_.Destroy(*device_);
    restir_di_.Destroy(*device_);
    virtual_texture_.Destroy(*device_);
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
    water_.reset();
    fluid_surface_.reset();
    ddgi_.reset();
    rcgi_.reset(); // owns GPU resources through device_; destroy before device
                   // teardown
    environment_.reset();
    material_system_.reset();
    bindless_.reset();
    transient_pool_.reset();
  }
  graph_.Reset();
  post_.reset();
  ui_blur_.reset(); // holds a Device& + backend handles; destroy before device_
  mesh_pipeline_.reset();
  swapchain_.reset();
  framegen_.reset(); // ffx contexts destroy through the device
  upscaler_.reset();
  raytracing_.reset();
  device_.reset();
}

const DeviceCaps *Renderer::caps() const {
  return device_ ? &device_->caps() : nullptr;
}

void Renderer::ClearFrameCallbacks() { graph_.Reset(); }

Format Renderer::swapchain_format() const {
  return swapchain_ ? swapchain_->format() : Format::kUnknown;
}

u32 Renderer::swapchain_image_count() const {
  return swapchain_ ? swapchain_->image_count() : 0;
}

} // namespace rx::render
