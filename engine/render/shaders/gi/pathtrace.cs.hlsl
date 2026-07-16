#include "rhi_bindings.hlsli"
// Progressive reference path tracer. Shares the scene's TLAS and bindless
// material/geometry tables with the realtime path; diffuse bounces with
// next-event estimation toward the sun and the procedural sky cube on miss.
// Accumulates one frame's samples into a persistent buffer that resets when
// the camera moves, so a still view converges to a ground-truth image.

struct PathPush {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;     // xyz eye
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w sun angular radius (radians)
  uint2 size;
  uint frame_index;
  uint sample_base;  // samples already accumulated (0 = overwrite)
  uint spp;          // samples this dispatch
  uint bounces;
  uint reset;
  uint pad;
};
PUSH_CONSTANTS(PathPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> output_image : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> accum_image : register(u1, space0);
[[vk::binding(2, 0)]] RaytracingAccelerationStructure tlas : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] TextureCube sky_cube : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState sky_sampler : register(s3, space0);

#define RX_GEOMETRY_SPACE space1
#include "rt_geometry.hlsli"
#include "material_record.hlsli"
#include "sss_profile.hlsli"
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);
[[vk::binding(2, 1)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space1);
[[vk::binding(3, 1)]] Texture2D bindless_textures[RX_BINDLESS_TEXTURE_COUNT] : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState bindless_sampler : register(s4, space1);

// Pristine reference: this brute-force ground-truth accumulator deliberately
// traces RAY_FLAG_FORCE_OPAQUE (no alpha-tested foliage), exactly as it did
// before the playable/denoised work. The denoised gbuffer pass owns the
// alpha-test + texture-lod improvements.
static const float kPi = 3.14159265359;

// pcg hash based rng in [0,1).
uint Pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float Rand(inout uint state) { return (Pcg(state) & 0xffffffu) / 16777216.0; }

float3 CosineHemisphere(float3 n, inout uint rng) {
  float u1 = Rand(rng);
  float u2 = Rand(rng);
  float r = sqrt(u1);
  float phi = 2.0 * kPi * u2;
  float3 t = abs(n.y) < 0.99 ? normalize(cross(float3(0, 1, 0), n))
                             : normalize(cross(float3(1, 0, 0), n));
  float3 b = cross(n, t);
  return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
}

struct Hit {
  bool hit;
  float3 position;
  float3 normal;
  float3 albedo;
  float3 emissive;
  bool skin;
  float3 sss_sigma_t;
  float3 sss_sigma_s;
  float3 sss_scatter_color;
};

Hit TraceClosest(float3 origin, float3 dir) {
  Hit h;
  h.hit = false;
  h.skin = false;
  h.sss_sigma_t = 0.0.xxx;
  h.sss_sigma_s = 0.0.xxx;
  h.sss_scatter_color = 0.0.xxx;
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.001;
  ray.Direction = dir;
  ray.TMax = 1000.0;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, RX_RAY_MASK_PATHTRACE, ray);
  rq.Proceed();
  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return h;

  h.hit = true;
  h.position = origin + dir * rq.CommittedRayT();
  MeshRecord mesh = mesh_records[NonUniformResourceIndex(rq.CommittedInstanceID())];
  GeometryRecord geometry = geometry_records[mesh.geometry_offset + rq.CommittedGeometryIndex()];
  uint3 tri =
      RxLoadTriangle(mesh, geometry.index_offset + rq.CommittedPrimitiveIndex() * 3);
  float2 bary = rq.CommittedTriangleBarycentrics();
  float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
  float3 n_local = 0.0.xxx;
  float2 uv = 0.0.xx;
  [unroll]
  for (uint c = 0; c < 3; ++c) {
    n_local += RxLoadNormal(mesh, tri[c]) * w[c];
    uv += RxLoadUv(mesh, tri[c]) * w[c];
  }
  float3x4 to_world = rq.CommittedObjectToWorld3x4();
  float3 n = normalize(mul((float3x3)to_world, n_local));
  if (dot(n, dir) > 0.0) n = -n;
  h.normal = n;

  MaterialRecord m = material_records[NonUniformResourceIndex(geometry.material_index)];
  float3 albedo = m.base_color_factor.rgb;
  if (m.base_color_texture != 0xffffffffu) {
    albedo *= bindless_textures[NonUniformResourceIndex(m.base_color_texture)]
                  .SampleLevel(bindless_sampler, uv, 0.0).rgb;
  }
  h.albedo = albedo;
  h.emissive = m.emissive;
  if ((m.flags & RX_MATERIAL_FLAG_SKIN) != 0u) {
    h.skin = true;
    h.sss_sigma_t = m.sss_sigma_t;
    h.sss_sigma_s = m.sss_sigma_s;
    h.sss_scatter_color = m.sss_scatter_color;
  }
  return h;
}

bool Occluded(float3 origin, float3 dir, float dist) {
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.001;
  ray.Direction = dir;
  ray.TMax = dist;
  RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, RX_RAY_MASK_PATHTRACE, ray);
  rq.Proceed();
  return rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

float3 SampleSky(float3 dir) {
  // Clamp suppresses the raw sun disk; direct sun comes from the nee term.
  return min(sky_cube.SampleLevel(sky_sampler, dir, 0.0).rgb, 6.0.xxx);
}

float3 SunDirection(inout uint rng) {
  float3 l = normalize(-pc.sun_direction.xyz);
  float radius = pc.sun_color.w;
  if (radius <= 0.0) return l;
  float3 up = abs(l.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
  float3 t1 = normalize(cross(up, l));
  float3 t2 = cross(l, t1);
  float a = 2.0 * kPi * Rand(rng);
  float r = sqrt(Rand(rng)) * radius;
  return normalize(l + t1 * (cos(a) * r) + t2 * (sin(a) * r));
}

// Diffusion BSSRDF single- + multiple-scattering at a skin hit (King 2013 disk
// importance sampling of the Christensen-Burley profile). Samples an entry
// point on the surface around the exit point, does next-event estimation there,
// and weights by the profile. One probe ray + one shadow ray per call. This is
// the diffusion baseline the SIGGRAPH-2025 hybrid and the KIT ReSTIR method
// build upon; here it runs brute force (the reference path tracer denoises by
// accumulation).
float3 SubsurfaceRadiance(Hit h, float3 sun, inout uint rng) {
  float3 d = SssBurleyScale(h.sss_sigma_t, h.sss_scatter_color);
  // Hero-wavelength channel selection, then sample a radius from its profile.
  int ch = min(int(Rand(rng) * 3.0), 2);
  float dch = (ch == 0) ? d.r : ((ch == 1) ? d.g : d.b);
  float r = SssSampleRadius(dch, Rand(rng));
  float phi = 2.0 * kPi * Rand(rng);
  float3 t, bt;
  SssBuildFrame(h.normal, t, bt);
  // Sample a disk around the exit point and project back onto the surface along
  // the normal to find the light-entry point.
  float3 disk = (t * cos(phi) + bt * sin(phi)) * r;
  float3 probe_origin = h.position + disk + h.normal * (r + 0.01);
  Hit e = TraceClosest(probe_origin, -h.normal);
  if (!e.hit) return 0.0.xxx;
  if (length(e.position - h.position) > 4.0 * max(d.r, max(d.g, d.b))) return 0.0.xxx;
  // Per-channel profile at the sampled radius, MIS pdf across the three channels
  // (each chosen with prob 1/3; the pdf already carries the disk jacobian).
  float3 Rd = float3(SssBurley(r, d.r), SssBurley(r, d.g), SssBurley(r, d.b));
  float pdf = (SssBurleyPdf(r, d.r) + SssBurleyPdf(r, d.g) + SssBurleyPdf(r, d.b)) / 3.0;
  // NEE toward the sun at the entry point.
  float3 ldir = SunDirection(rng);
  float ndl = dot(e.normal, ldir);
  float3 E = 0.0.xxx;
  if (ndl > 0.0 && !Occluded(e.position + e.normal * 0.002, ldir, 1000.0))
    E = sun * ndl;
  return saturate(h.sss_scatter_color) * Rd * E / max(pdf, 1e-6);
}

float3 Radiance(float3 origin, float3 dir, inout uint rng) {
  float3 throughput = 1.0.xxx;
  float3 radiance = 0.0.xxx;
  float3 sun = pc.sun_color.rgb * pc.sun_direction.w;
  for (uint b = 0; b < pc.bounces; ++b) {
    Hit h = TraceClosest(origin, dir);
    if (!h.hit) {
      radiance += throughput * SampleSky(dir);
      break;
    }
    radiance += throughput * h.emissive;

    if (h.skin) {
      // Subsurface: direct light enters nearby and diffuses to the exit point.
      radiance += throughput * SubsurfaceRadiance(h, sun, rng);
      // Indirect: continue with a diffuse bounce weighted by the scatter colour.
      dir = CosineHemisphere(h.normal, rng);
      origin = h.position + h.normal * 0.002;
      throughput *= saturate(h.sss_scatter_color);
    } else {
      // Next event estimation toward the (soft) sun disk.
      float3 ldir = SunDirection(rng);
      float ndl = dot(h.normal, ldir);
      if (ndl > 0.0 && !Occluded(h.position + h.normal * 0.002, ldir, 1000.0)) {
        radiance += throughput * h.albedo / kPi * sun * ndl;
      }

      // Diffuse bounce; the cosine pdf cancels the albedo/pi * ndl factors.
      dir = CosineHemisphere(h.normal, rng);
      origin = h.position + h.normal * 0.002;
      throughput *= h.albedo;
    }

    // Russian-roulette-free fixed depth; kill near-black paths early.
    if (max(throughput.r, max(throughput.g, throughput.b)) < 0.01) break;
  }
  return radiance;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  uint rng = (id.y * pc.size.x + id.x) * 9781u + pc.frame_index * 26699u + 1u;

  float3 sum = 0.0.xxx;
  for (uint s = 0; s < pc.spp; ++s) {
    float2 jitter = float2(Rand(rng), Rand(rng));
    float2 ndc = (float2(id.xy) + jitter) / float2(pc.size) * 2.0 - 1.0;
    // Reversed infinite z: depth 1 is the near plane (finite w), depth 0 is at
    // infinity (w -> 0), so reconstruct the near point and aim from the eye.
    float4 near_h = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
    float3 p_near = near_h.xyz / near_h.w;
    float3 ro = pc.camera_pos.xyz;
    float3 rd = normalize(p_near - ro);
    sum += Radiance(ro, rd, rng);
  }

  float total = float(pc.sample_base + pc.spp);
  float3 accumulated = (pc.reset != 0u) ? sum : accum_image[id.xy].rgb + sum;
  accum_image[id.xy] = float4(accumulated, total);
  output_image[id.xy] = float4(accumulated / max(total, 1.0), 1.0);
}
