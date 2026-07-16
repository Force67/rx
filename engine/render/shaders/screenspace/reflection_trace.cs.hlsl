#include "rhi_bindings.hlsli"
// Stochastic specular reflections for the hybrid renderer: one VNDF-sampled
// GGX ray per pixel through the scene TLAS, shaded like the old inline mirror
// ray (sun + nearest-probe DDGI + emissive at the hit, prefiltered sky on
// miss) PLUS a sun shadow ray at the hit, packed for NRD REBLUR_SPECULAR.
// Replaces the forward pass's deterministic mirror ray: glossy surfaces get a
// real distribution instead of a mirror-to-IBL crossfade, and the denoiser
// eats the sky-hit sparkle foliage used to mint.
//
// NRD.hlsli supplies the radiance packing; keep the CI fallback compiling.
#if __has_include("NRD.hlsli")
#include "NRD.hlsli"
#else
float REBLUR_FrontEnd_GetNormHitDist(float hit_dist, float view_z, float3 params,
                                     float roughness) {
  float smc = (1.0 - exp2(-200.0 * roughness * roughness)) * pow(saturate(roughness), 0.5);
  float f = (params.x + abs(view_z) * params.y) * lerp(params.z, 1.0, smc);
  return max(saturate(hit_dist / f), 1e-6);
}
float4 REBLUR_FrontEnd_PackRadianceAndNormHitDist(float3 radiance, float norm_hit_dist,
                                                  bool sanitize) {
  return float4(radiance, norm_hit_dist);
}
#endif

struct ReflectionPush {
  column_major float4x4 inv_view_proj;  // unjittered
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb
  float4 fog;            // x density, y height falloff, z base height, w unused
  float2 inv_size;       // full-res inverse size (guides are always full res)
  float near_plane;
  float roughness_cutoff;  // above: no trace, ibl only (packed as 0)
  float hit_a;             // REBLUR hit-dist params A, B, C
  float hit_b;
  float hit_c;
  float frame_index;
  float max_ray_dist;      // base reflection reach, roughness-scaled at trace time
  float sh_skip_rough;     // roughness above which the trace is replaced by the RCGI SH
  float sh_dir_thresh;     // dot(rayDir, mirrorDir) below which the trace is SH-skipped
  float fog_mip;           // prefiltered-cube mip used as the fog inscatter colour
  uint4 dims;   // x out_w, y out_h, z sh_gather_w, w sh_gather_h
  uint4 misc;   // x guide sample step (1 full, 2 half), y flags, zw pad
};
PUSH_CONSTANTS(ReflectionPush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> out_radiance : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> normal_map : register(t2, space0);  // oct rg, roughness b
[[vk::binding(3, 0)]] RaytracingAccelerationStructure tlas : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] TextureCube prefiltered_cube : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState prefiltered_sampler : register(s4, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2DArray ddgi_irradiance : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState ddgi_irradiance_sampler : register(s5, space0);
struct DdgiVolume {
  float4 origin;  // xyz grid origin, w probe spacing
  uint4 counts;   // xyz probe counts, w irradiance texel resolution
  float4 params;  // x distance texels, y hysteresis, z max ray dist, w energy
};
[[vk::binding(6, 0)]] ConstantBuffer<DdgiVolume> ddgi : register(b6, space0);
// RCGI per-pixel diffuse SH (2-band, one float4 per colour channel), gather-res.
// Sampled by the specular ray-skip path (kFlagShSkip) to replace the TLAS trace
// on rough / off-mirror rays. Bound to a harmless placeholder when RCGI is off.
[[vk::binding(7, 0)]] Texture2D<float4> rcgi_sh_r : register(t7, space0);
[[vk::binding(8, 0)]] Texture2D<float4> rcgi_sh_g : register(t8, space0);
[[vk::binding(9, 0)]] Texture2D<float4> rcgi_sh_b : register(t9, space0);
#include "gi/sh.hlsli"
#include "gi/rcgi_common.hlsli"
// RCGI irradiance cascades for the spec-bounce indirect term (kFlagRcgi): under
// RCGI the DDGI atlas is empty, so the diffuse bounce at a reflection hit reads
// the RCGI cascades instead (SampleRcgiIrradiance). Bound to harmless
// placeholders when RCGI is off (flag stays clear, never sampled).
[[vk::binding(10, 0)]] ConstantBuffer<RcgiGlobals> rcgi_globals : register(b10, space0);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]] Texture2D rcgi_irr : register(t11, space0);
[[vk::combinedImageSampler]] [[vk::binding(11, 0)]] SamplerState rcgi_irr_smp : register(s11, space0);
[[vk::combinedImageSampler]] [[vk::binding(12, 0)]] Texture2D rcgi_vis : register(t12, space0);
[[vk::combinedImageSampler]] [[vk::binding(12, 0)]] SamplerState rcgi_vis_smp : register(s12, space0);
[[vk::binding(13, 0)]] StructuredBuffer<uint2> rcgi_probe_meta : register(t13, space0);
[[vk::binding(14, 0)]] StructuredBuffer<float4> rcgi_interior_vols : register(t14, space0);

// Bindless scene tables (set 1, shared layout with the forward rt variant).
#define RX_GEOMETRY_SPACE space1
#include "rt_geometry.hlsli"
#include "material_record.hlsli"
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D bindless_textures[RX_BINDLESS_TEXTURE_COUNT] : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState bindless_sampler : register(s4, space1);

static const float kPi = 3.14159265359;
static const uint kFlagDdgi = 1u;
// RX_RT_VEG_ANYHIT: evaluate the real alpha texture on masked (vegetation) hits
// via a bounded any-hit loop instead of the force-opaque approximation, which
// is visibly wrong in sharp reflections (AC Shadows finding). Capped at
// kMaxAlphaTests candidate evaluations; beyond that the next candidate commits.
static const uint kFlagVegAnyHit = 2u;
static const uint kMaxAlphaTests = 4u;
// RX_REFL_FOG: integrate exponential height fog once over the reflected segment.
static const uint kFlagFog = 4u;
// RX_REFL_SH_SKIP: replace the TLAS trace with the RCGI per-pixel diffuse SH on
// rough / off-mirror rays (evaluated along the ray dir). Requires RCGI active.
static const uint kFlagShSkip = 8u;
// RCGI active: read the spec-bounce indirect-diffuse term from the RCGI
// irradiance cascades instead of the DDGI atlas (empty under RCGI).
static const uint kFlagRcgi = 16u;

// Whether a non-opaque (masked) candidate hit survives its alpha test: the
// material's base-color alpha at the interpolated UV vs the material cutoff.
bool CandidateAlphaPasses(uint instance_id, uint geometry_index, uint primitive_index,
                          float2 bary) {
  MeshRecord mesh = mesh_records[NonUniformResourceIndex(instance_id)];
  GeometryRecord geometry = geometry_records[mesh.geometry_offset + geometry_index];
  uint3 tri = RxLoadTriangle(mesh, geometry.index_offset + primitive_index * 3);
  float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
  float2 huv = RxLoadUv(mesh, tri[0]) * w[0] + RxLoadUv(mesh, tri[1]) * w[1] +
               RxLoadUv(mesh, tri[2]) * w[2];
  MaterialRecord m = material_records[NonUniformResourceIndex(geometry.material_index)];
  float alpha = m.base_color_factor.a;
  if (m.base_color_texture != 0xffffffffu) {
    alpha *= bindless_textures[NonUniformResourceIndex(m.base_color_texture)]
                 .SampleLevel(bindless_sampler, huv, 0.0).a;
  }
  return alpha >= m.alpha_cutoff;
}

float3 OctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}
float2 OctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}

float Ign(float2 pixel, float offset) {
  float ign = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
  return frac(ign + offset * 0.61803398875);
}

// Heitz 2018 VNDF sampling: a GGX-visible-normal half vector around n for
// view v, driving the reflection direction. alpha = roughness^2.
float3 SampleGgxVndf(float3 v, float3 n, float alpha, float2 u) {
  float3 up = abs(n.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  float3 t = normalize(cross(up, n));
  float3 b = cross(n, t);
  float3 ve = float3(dot(v, t), dot(v, b), dot(v, n));

  float3 vh = normalize(float3(alpha * ve.x, alpha * ve.y, ve.z));
  float lensq = vh.x * vh.x + vh.y * vh.y;
  float3 t1 = lensq > 0.0 ? float3(-vh.y, vh.x, 0.0) / sqrt(lensq) : float3(1, 0, 0);
  float3 t2 = cross(vh, t1);
  float r = sqrt(u.x);
  float phi = 2.0 * kPi * u.y;
  float p1 = r * cos(phi);
  float p2 = r * sin(phi);
  float s = 0.5 * (1.0 + vh.z);
  p2 = (1.0 - s) * sqrt(1.0 - p1 * p1) + s * p2;
  float3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
  float3 h = normalize(float3(alpha * nh.x, alpha * nh.y, max(0.0, nh.z)));
  return normalize(h.x * t + h.y * b + h.z * n);
}

float2 ProbeAtlasUv(uint3 probe, float3 dir, float texels, float2 atlas_size) {
  float2 oct = OctEncode(dir) * 0.5 + 0.5;
  float2 base = float2(probe.x + probe.z * ddgi.counts.x, probe.y) * (texels + 2.0) + 1.0;
  return (base + 0.5 + oct * (texels - 1.0)) / atlas_size;
}

// Nearest-probe diffuse gi at the reflection hit (matches the forward rt
// variant's SampleDdgiNearest).
float3 SampleDdgiNearest(float3 world_pos, float3 n) {
  if ((pc.misc.y & kFlagDdgi) == 0u) return 0.0.xxx;
  float3 local = (world_pos - ddgi.origin.xyz) / ddgi.origin.w;
  if (any(local < 0.0) || any(local > float3(ddgi.counts.xyz - 1))) return 0.0.xxx;
  uint3 probe = (uint3)round(local);
  float texels = (float)ddgi.counts.w;
  float2 atlas = float2((ddgi.counts.w + 2) * ddgi.counts.x * ddgi.counts.z,
                        (ddgi.counts.w + 2) * ddgi.counts.y);
  float3 irr = ddgi_irradiance
      .SampleLevel(ddgi_irradiance_sampler,
                   float3(ProbeAtlasUv(probe, n, texels, atlas), 0.0), 0.0).rgb;
  return irr * irr * ddgi.params.w;
}

// Indirect diffuse irradiance at a reflection hit point. Under RCGI the DDGI
// atlas is empty, so read the RCGI cascades (leak-hardened classification, item
// 20b); otherwise fall back to the nearest DDGI probe.
float3 SampleHitIndirect(float3 world_pos, float3 n, float3 v) {
  if ((pc.misc.y & kFlagRcgi) != 0u) {
    return SampleRcgiIrradiance(rcgi_globals, rcgi_irr, rcgi_irr_smp, rcgi_vis, rcgi_vis_smp,
                                rcgi_probe_meta, rcgi_interior_vols, world_pos, n, v);
  }
  return SampleDdgiNearest(world_pos, n);
}

bool SunOccluded(float3 origin, float3 dir) {
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.02;
  ray.Direction = dir;
  ray.TMax = 500.0;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, RX_RAY_MASK_REALTIME, ray);
  rq.Proceed();
  return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// Evaluate the RCGI per-pixel diffuse SH along `dir` for the ray-skip path.
// Gather-res SH; map the (possibly half-res) reflection pixel to a gather texel
// through its full-res uv so any output/gather ratio works.
float3 SampleRcgiShAlong(float2 full_uv, float3 dir) {
  int2 sh = clamp(int2(full_uv * float2(pc.dims.zw)), int2(0, 0), int2(pc.dims.zw) - 1);
  Sh2 s;
  s.r = rcgi_sh_r.Load(int3(sh, 0));
  s.g = rcgi_sh_g.Load(int3(sh, 0));
  s.b = rcgi_sh_b.Load(int3(sh, 0));
  return max(ShEvaluate(s, dir), 0.0.xxx);
}

// Analytic exponential-height-fog transmittance over a reflected segment of
// length `dist` starting at `origin` travelling `dir` (RX_REFL_FOG). The primary
// surface already carries the eye->surface fog, so we integrate only the extra
// reflected path (skips near-camera fog by construction).
float HeightFogTransmittance(float3 origin, float3 dir, float dist) {
  float density = pc.fog.x;
  float b = pc.fog.y;      // height falloff
  float base = pc.fog.z;   // base plane height
  float d0 = density * exp(-max(0.0, origin.y - base) * b);
  float bdy = b * dir.y;
  float tau = (abs(bdy) > 1e-4) ? d0 * (1.0 - exp(-bdy * dist)) / bdy : d0 * dist;
  return exp(-max(tau, 0.0));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.dims.x || id.y >= pc.dims.y) return;
  // Guides are full-res; map the (possibly half-res) output pixel to a full-res
  // guide texel by the sample step (1 = full, 2 = half). Jitter the sub-pixel
  // choice per frame so the half-res selection does not lock onto one quadrant.
  uint step = max(pc.misc.x, 1u);
  uint2 sub = (step > 1u) ? uint2(uint(pc.frame_index) & 1u, (uint(pc.frame_index) >> 1) & 1u)
                          : uint2(0, 0);
  // Clamp to the full-res guide extent: on odd render dimensions id.xy*step+sub
  // can step one texel past the edge, an out-of-bounds Load. The upscale carries
  // this same subpixel so both passes weight against the identical guide texel.
  uint gw, gh;
  depth_map.GetDimensions(gw, gh);
  int2 fp = clamp(int2(id.xy * step + sub), int2(0, 0), int2(gw, gh) - 1);
  int3 p = int3(fp, 0);

  float depth = depth_map.Load(p);
  float4 nr = normal_map.Load(p);
  float roughness = nr.b;
  if (depth <= 0.0 || roughness > pc.roughness_cutoff) {  // sky / ibl-only
    out_radiance[id.xy] = 0.0.xxxx;
    return;
  }

  float2 uv = (float2(fp) + 0.5) * pc.inv_size;
  float2 ndc = uv * 2.0 - 1.0;  // vulkan y-down clip, matches rtao/fog
  float4 world_h = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
  float3 world = world_h.xyz / world_h.w;
  float view_z = pc.near_plane / depth;

  float3 n = OctDecode(nr.rg);
  float3 v = normalize(pc.camera_pos.xyz - world);
  // Double-sided foliage: the prepass exports the geometric facing, which
  // points away from the camera on back faces; VNDF sampling needs v above
  // the surface or it degenerates into fireflies.
  if (dot(n, v) < 0.0) n = -n;
  float2 u = float2(Ign(float2(fp), pc.frame_index),
                    Ign(float2(fp) + 17.0, pc.frame_index * 1.618));
  float alpha = roughness * roughness;
  float3 h = SampleGgxVndf(v, n, alpha, u);
  float3 dir = reflect(-v, h);
  if (dot(dir, n) <= 0.0) dir = reflect(dir, n);  // rare below-horizon sample

  // Roughness-scaled ray reach (AC Shadows): rough surfaces reflect only the
  // near neighbourhood, so shorten the ray -- fewer long traversals, and the
  // miss (sky/SH) fills the far field the blur would swallow anyway.
  float ray_reach = pc.max_ray_dist * ((1.0 - roughness) * (1.0 - roughness) + 0.1);

  // Specular ray-skip (RX_REFL_SH_SKIP): on rough or off-mirror rays the traced
  // radiance is indistinguishable from the diffuse field once denoised, so read
  // the RCGI per-pixel SH along the ray instead of tracing. Keeps the hard
  // roughness_cutoff as the outer bound; this is the softer, directional cull.
  float3 mirror = reflect(-v, n);
  bool sh_skip = (pc.misc.y & kFlagShSkip) != 0u &&
                 (roughness > pc.sh_skip_rough || dot(dir, mirror) < pc.sh_dir_thresh);
  if (sh_skip) {
    float3 rad = SampleRcgiShAlong(uv, dir);
    // A short normalised hit distance keeps REBLUR treating these as diffuse-ish
    // near hits (no long-range specular history reuse across the skip).
    float norm = REBLUR_FrontEnd_GetNormHitDist(ray_reach * 0.25, view_z,
                                                float3(pc.hit_a, pc.hit_b, pc.hit_c), roughness);
    out_radiance[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(rad, norm, true);
    return;
  }

  RayDesc ray;
  ray.Origin = world + n * 0.02;
  ray.TMin = 0.02;
  ray.Direction = dir;
  ray.TMax = ray_reach;
  // Specular reflections keep the real masked (vegetation) geometry -- the
  // opaque approximation reads wrong in sharp reflections. Masked triangles are
  // non-opaque in the tlas; run the RayQuery candidate loop and alpha-test them
  // against the real texture, capped at kMaxAlphaTests evaluations. With the
  // any-hit path off (RX_RT_VEG_ANYHIT=0 or RX_RT_VEG=0) every candidate commits
  // immediately, reproducing the old force-opaque behavior.
  bool veg_anyhit = (pc.misc.y & kFlagVegAnyHit) != 0u;
  RayQuery<RAY_FLAG_NONE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, RX_RAY_MASK_REALTIME, ray);
  uint alpha_tests = 0u;
  while (rq.Proceed()) {
    if (rq.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
      bool commit = true;
      if (veg_anyhit && alpha_tests < kMaxAlphaTests) {
        ++alpha_tests;
        commit = CandidateAlphaPasses(rq.CandidateInstanceID(), rq.CandidateGeometryIndex(),
                                      rq.CandidatePrimitiveIndex(),
                                      rq.CandidateTriangleBarycentrics());
      }
      if (commit) rq.CommitNonOpaqueTriangleHit();
    }
  }

  float3 radiance;
  float hit_t = ray_reach;
  bool hit = rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
  if (!hit) {
    radiance = min(prefiltered_cube.SampleLevel(prefiltered_sampler, dir, 1.0).rgb, 8.0.xxx);
  } else {
    hit_t = rq.CommittedRayT();
    float3 hit_pos = ray.Origin + dir * hit_t;
    MeshRecord mesh = mesh_records[NonUniformResourceIndex(rq.CommittedInstanceID())];
    GeometryRecord geometry =
        geometry_records[mesh.geometry_offset + rq.CommittedGeometryIndex()];
    uint3 tri =
        RxLoadTriangle(mesh, geometry.index_offset + rq.CommittedPrimitiveIndex() * 3);
    float2 bary = rq.CommittedTriangleBarycentrics();
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 n_local = 0.0.xxx;
    float2 huv = 0.0.xx;
    [unroll]
    for (uint corner = 0; corner < 3; ++corner) {
      n_local += RxLoadNormal(mesh, tri[corner]) * w[corner];
      huv += RxLoadUv(mesh, tri[corner]) * w[corner];
    }
    float3x4 to_world = rq.CommittedObjectToWorld3x4();
    float3 hit_n = normalize(mul((float3x3)to_world, n_local));
    if (dot(hit_n, dir) > 0.0) hit_n = -hit_n;

    MaterialRecord hit_material =
        material_records[NonUniformResourceIndex(geometry.material_index)];
    float3 albedo = hit_material.base_color_factor.rgb;
    if (hit_material.base_color_texture != 0xffffffffu) {
      albedo *= bindless_textures[NonUniformResourceIndex(hit_material.base_color_texture)]
                    .SampleLevel(bindless_sampler, huv, 2.0).rgb;
    }
    float3 to_sun = normalize(-pc.sun_direction.xyz);
    float3 sun = pc.sun_color.rgb * pc.sun_direction.w;
    float ndl = max(dot(hit_n, to_sun), 0.0);
    // The old inline ray skipped this shadow ray; shadowed geometry no longer
    // reflects as if sunlit.
    float visible = (ndl > 0.0 && !SunOccluded(hit_pos + hit_n * 0.02, to_sun)) ? 1.0 : 0.0;
    radiance = albedo / kPi * sun * ndl * visible +
               albedo * SampleHitIndirect(hit_pos, hit_n, -dir) + hit_material.emissive;
  }

  // One-step fog on the reflected segment (RX_REFL_FOG): fade the hit radiance
  // toward the horizon inscatter colour so distant reflections do not cut hard
  // where the raster fog would have swallowed them. Misses already sample the
  // sky cube (= the fog colour at the horizon), so only shade the hit path.
  if (hit && (pc.misc.y & kFlagFog) != 0u) {
    float t = HeightFogTransmittance(ray.Origin, dir, hit_t);
    float3 inscatter =
        min(prefiltered_cube.SampleLevel(prefiltered_sampler, dir, pc.fog_mip).rgb, 8.0.xxx);
    radiance = radiance * t + inscatter * (1.0 - t);
  }

  float norm_hit = REBLUR_FrontEnd_GetNormHitDist(hit_t, view_z,
                                                  float3(pc.hit_a, pc.hit_b, pc.hit_c),
                                                  roughness);
  out_radiance[id.xy] = REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, norm_hit, true);
}
