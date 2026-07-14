#ifndef RX_GI_SDF_TRACE_HLSLI_
#define RX_GI_SDF_TRACE_HLSLI_

// Global SDF clipmap sphere-trace -- the software visibility ray S2 swaps in for
// a TLAS query on hardware without ray query (the Lumen "software ray tracing"
// idea grafted onto RCGI's visibility-only rays). Dependency-free: no RayQuery,
// no bindless tables. All resources are passed as parameters (rcgi_common.hlsli
// convention), so an includer declares its own bindings.
//
// ============================ S2 binding contract ============================
// A pass that traces the clipmap must bind (any slots; pass them in):
//   ConstantBuffer<SdfGlobals> sdf   -- clip origins / voxel sizes / camera.
//   Texture3D<float>  sdf_distance   -- signed distance, R16Float, kGeneral.
//   Texture3D<float4> sdf_albedo     -- surface albedo proxy, RGBA8, kGeneral.
//   Texture3D<float4> sdf_emissive   -- surface emissive proxy, RGBA8, kGeneral.
//   SamplerState      sdf_sampler    -- linear clamp-to-edge (all three axes).
// The three volumes are one stacked-Z atlas of size
//   (kSdfRes, kSdfRes, kSdfRes * kSdfClips): clip c occupies z in
//   [c*kSdfRes, (c+1)*kSdfRes). Bind them InGeneral (they live in kGeneral).
//
// Entry point:
//   SdfHit TraceGlobalSdf(origin, dir, tmax, sdf, sdf_distance, sdf_albedo,
//                         sdf_emissive, sdf_sampler)
//   -> sphere-traces the finest clip containing the ray, stepping up clips as
//      the ray leaves them; hit when distance < surface epsilon (voxel-scaled);
//      6-tap central-difference gradient normal; ~1-voxel start bias. Returns
//      hit position, normal, albedo, emissive, hitT, and a miss flag.
// =============================================================================

static const uint kSdfClips = 4u;
static const uint kSdfRes = 128u;  // per-axis voxels per clip (mirror SdfClipmap::kRes)

struct SdfGlobals {
  float4 clip_origin[kSdfClips];  // xyz world min corner of the clip, w voxel size (m)
  float4 clip_params;             // x res, y clip count, z total_z (res*clips), w far distance
  float4 camera_pos;              // xyz eye, w unused
};

// local voxel coordinate [0,res] of `world` inside clip c (no clamp).
float3 SdfLocal(SdfGlobals g, uint c, float3 world) {
  return (world - g.clip_origin[c].xyz) / g.clip_origin[c].w;
}

// true when `world` sits inside clip c with `margin` voxels to spare on each side.
bool SdfInsideClip(SdfGlobals g, uint c, float3 world, float margin) {
  float3 l = SdfLocal(g, c, world);
  float res = g.clip_params.x;
  return all(l >= margin) && all(l <= res - margin);
}

// finest (smallest) clip that contains `world` with a 1-voxel guard band; returns
// kSdfClips when the point is outside every clip.
uint SdfSelectClip(SdfGlobals g, float3 world) {
  [unroll]
  for (uint c = 0; c < kSdfClips; ++c) {
    if (SdfInsideClip(g, c, world, 1.0)) return c;
  }
  return kSdfClips;
}

// texture uvw of `world` in clip c's z-slab, clamped so a trilinear tap never
// bleeds into a neighbouring clip's slab or off the volume.
float3 SdfClipUvw(SdfGlobals g, uint c, float3 world) {
  float res = g.clip_params.x;
  float total = g.clip_params.z;
  float3 l = clamp(SdfLocal(g, c, world), 0.5, res - 0.5);
  float3 uvw;
  uvw.xy = l.xy / res;
  uvw.z = (l.z + c * kSdfRes) / total;
  return uvw;
}

float SdfDistance(SdfGlobals g, uint c, float3 world, Texture3D<float> vol, SamplerState smp) {
  return vol.SampleLevel(smp, SdfClipUvw(g, c, world), 0.0).r;
}

// Ray/AABB slab test. Returns true when [origin+dir*t] enters [bmin,bmax] with
// the near/far parameters in t_near/t_far (t_near may be negative when origin is
// already inside the box).
bool SdfRayBox(float3 origin, float3 dir, float3 bmin, float3 bmax, out float t_near,
               out float t_far) {
  float3 inv = 1.0 / dir;  // dir is normalized; a zero component yields +/-inf, folded by min/max
  float3 t0 = (bmin - origin) * inv;
  float3 t1 = (bmax - origin) * inv;
  float3 tsmall = min(t0, t1), tbig = max(t0, t1);
  t_near = max(max(tsmall.x, tsmall.y), tsmall.z);
  t_far = min(min(tbig.x, tbig.y), tbig.z);
  return t_far >= max(t_near, 0.0);
}

struct SdfHit {
  float3 pos;
  float3 normal;
  float3 albedo;
  float3 emissive;
  float hitT;
  bool miss;
  bool inside;  // ray origin sits inside closed geometry (SDF<0 at the start) -->
                // the hardware "backface" case; the caller must skip cache insertion.
};

// `start_t` is the self-hit start bias / initial step. Scale it with the voxel
// size of the clip the ray origin sits in: a fixed finest-clip bias self-
// intersects a coarse-cascade surface (whose hit epsilon is much larger), which
// is why the sun-occlusion trace passes the hit clip's voxel here rather than
// clip 0's (see rcgi_cache_shade_body.hlsli).
SdfHit TraceGlobalSdf(float3 origin, float3 dir, float tmax, float start_t, SdfGlobals g,
                      Texture3D<float> sdf_distance, Texture3D<float4> sdf_albedo,
                      Texture3D<float4> sdf_emissive, SamplerState sdf_sampler) {
  SdfHit hit;
  hit.pos = origin;
  hit.normal = float3(0, 0, 0);
  hit.albedo = float3(0, 0, 0);
  hit.emissive = float3(0, 0, 0);
  hit.hitT = tmax;
  hit.miss = true;
  hit.inside = false;

  float t = start_t;  // start bias ~1 voxel of the origin's clip, to dodge self-hits
  bool sampled = false;  // whether we have taken a real (in-clipmap) distance sample yet

  [loop]
  for (int i = 0; i < 256; ++i) {
    if (t > tmax) break;
    float3 p = origin + dir * t;
    uint c = SdfSelectClip(g, p);
    if (c >= kSdfClips) {
      // Outside every clip's guarded volume. An outer RCGI probe can sit on the
      // coarsest clip boundary, and the small initial step cannot bridge the
      // coarse voxel gap to the guard band; slab-test the coarsest guarded AABB
      // and jump to the entry point rather than reporting a false miss. Clips are
      // nested (coarser fully contains finer), so once we have been inside, an
      // exit past the coarsest bound is final.
      if (sampled) break;
      uint cc = kSdfClips - 1u;
      float cvoxel = g.clip_origin[cc].w;
      float3 bmin = g.clip_origin[cc].xyz + cvoxel;                        // 1-voxel guard inset
      float3 bmax = g.clip_origin[cc].xyz + cvoxel * (g.clip_params.x - 1.0);
      float t_near, t_far;
      if (SdfRayBox(origin, dir, bmin, bmax, t_near, t_far) && t_near > t && t_near < tmax) {
        t = t_near + cvoxel;  // step a voxel inside so SdfSelectClip's guard band accepts it
        continue;
      }
      break;  // never enters the clipmap: a true miss
    }

    float voxel = g.clip_origin[c].w;
    float d = SdfDistance(g, c, p, sdf_distance, sdf_sampler);
    float eps = voxel * 0.75;

    // The very first in-clipmap sample being negative means the ray started
    // inside closed geometry: mirror the hardware backface case (no hit, no cache
    // insertion) instead of returning a spurious positive-distance hit.
    if (!sampled && d < 0.0) {
      hit.inside = true;
      hit.miss = false;
      hit.hitT = t;
      hit.pos = p;
      return hit;
    }
    sampled = true;

    if (d < eps) {  // surface hit
      hit.miss = false;
      hit.hitT = t;
      hit.pos = p;
      // 6-tap central-difference gradient in this clip.
      float h = voxel;
      float dx = SdfDistance(g, c, p + float3(h, 0, 0), sdf_distance, sdf_sampler) -
                 SdfDistance(g, c, p - float3(h, 0, 0), sdf_distance, sdf_sampler);
      float dy = SdfDistance(g, c, p + float3(0, h, 0), sdf_distance, sdf_sampler) -
                 SdfDistance(g, c, p - float3(0, h, 0), sdf_distance, sdf_sampler);
      float dz = SdfDistance(g, c, p + float3(0, 0, h), sdf_distance, sdf_sampler) -
                 SdfDistance(g, c, p - float3(0, 0, h), sdf_distance, sdf_sampler);
      float3 grad = float3(dx, dy, dz);
      hit.normal = length(grad) > 1e-6 ? normalize(grad) : -dir;
      float3 uvw = SdfClipUvw(g, c, p);
      hit.albedo = sdf_albedo.SampleLevel(sdf_sampler, uvw, 0.0).rgb;
      hit.emissive = sdf_emissive.SampleLevel(sdf_sampler, uvw, 0.0).rgb;
      break;
    }
    // Sphere-trace step, floored to keep progress through coarse/flat regions.
    t += max(d, eps * 0.5);
  }
  return hit;
}

#endif  // RX_GI_SDF_TRACE_HLSLI_
