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

// true when `world` sits inside clip c's PHYSICAL voxel volume (local [0,res], no
// guard inset). Zero-margin variant of SdfInsideClip: used to classify origins that
// fall in the coarsest clip's one-voxel guard shell (where SdfSelectClip rejects
// them) but are still enclosed by the clip's data.
bool SdfInsidePhysical(SdfGlobals g, uint c, float3 world) {
  float3 l = SdfLocal(g, c, world);
  float res = g.clip_params.x;
  return all(l >= 0.0) && all(l <= res);
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

// `start_t` is the distance of the FIRST marched sample from the origin (applied
// only after the origin itself is classified at t=0). It is a pure march offset,
// not a self-hit bias: callers that need to clear their own surface offset the
// origin instead (the sun-occlusion trace in rcgi_cache_shade_body.hlsli pushes
// the origin ~1.5 voxels along the normal, then passes a tiny start_t here so a
// blocker just past that biased origin is not skipped -- one bias, not two).
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

  // Origin classification (at t=0, BEFORE the march bias). `start_t` shifts the
  // first sample ~1 voxel down the ray; testing "inside" only there would mislabel
  // a probe/point that starts just outside a surface firing inward (it can cross
  // in within start_t and read negative -> false backface) or just inside firing
  // outward (it can cross out before start_t and never see the negative -> false
  // miss). So sample the origin itself: if the field is negative there, the ray
  // starts inside closed geometry -> mirror the hardware backface case immediately.
  //
  // SdfSelectClip guard-bands by one voxel, so an OUTER probe sitting in the
  // coarsest clip's guard shell -- or exactly on a clip face, where the RCGI-
  // aligned clip snap (sdf_clipmap.cc) parks cascade-3 outer probes -- is rejected
  // by it even when the clip's data encloses the probe. For that case fall back to
  // the coarsest clip's PHYSICAL voxel extent (local [0,res], no guard inset) and
  // sample it with coords clamped to the valid trilinear interior (SdfClipUvw
  // clamps to [0.5,res-0.5]); a negative there is still inside/backface. Finer
  // clips keep their normal guarded sample -- the physical fallback is only for the
  // guard shell / boundary. Origins outside the coarsest physical extent are by
  // definition not inside geometry; fall through to the slab-entry march.
  uint c0 = SdfSelectClip(g, origin);
  uint classify_clip = c0;
  if (c0 >= kSdfClips && SdfInsidePhysical(g, kSdfClips - 1u, origin)) {
    classify_clip = kSdfClips - 1u;
  }
  bool origin_classified = classify_clip < kSdfClips;
  if (origin_classified &&
      SdfDistance(g, classify_clip, origin, sdf_distance, sdf_sampler) < 0.0) {
    hit.inside = true;
    hit.miss = false;
    hit.hitT = 0.0;
    hit.pos = origin;
    return hit;
  }

  float t = start_t;         // march bias, applied only after origin classification
  // Lower bound for bisecting a front crossing. `real_anchor` marks t_anchor as a
  // proven-outside FIELD sample (the classified origin here, just shown >= 0, or a
  // later positive march step) as opposed to the merely geometric slab-entry
  // boundary. Only a real anchor makes a subsequent negative a front-surface
  // crossing to bisect; a negative reached with the slab boundary as the only
  // anchor means the entry point is itself inside geometry -> backface.
  float t_anchor = 0.0;
  bool has_anchor = origin_classified;   // origin fell through as >= 0
  bool real_anchor = origin_classified;
  bool sampled = false;      // whether we have taken a real (in-clipmap) sample yet

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
        // Enter at the guard boundary plus a numerical nudge, NOT a full voxel in:
        // the boundary (local coordinate 1) is already accepted inclusively by
        // SdfInsideClip, so a +cvoxel step would skip an entire first valid voxel
        // (2 m in clip 3) and could stride past a front surface in it. 1% of the
        // coarsest voxel clears the float boundary without skipping geometry.
        t = t_near + cvoxel * 0.01;
        if (!has_anchor) {
          // The slab entry boundary is only a geometric guess, not a field sample:
          // record it as the anchor but leave real_anchor false, so a negative on
          // the first in-volume sample trips the backface backstop below rather
          // than bisecting to a fake front hit. (If the origin already gave a real
          // anchor -- an outer probe in the guard shell that read >= 0 -- keep it.)
          t_anchor = t;
          has_anchor = true;
        }
        continue;
      }
      break;  // never enters the clipmap: a true miss
    }

    float voxel = g.clip_origin[c].w;
    float d = SdfDistance(g, c, p, sdf_distance, sdf_sampler);
    float eps = voxel * 0.75;
    sampled = true;

    if (d < eps) {  // surface hit (d<0 too: crossed since the origin was outside)
      float t_hit = t;
      if (d < 0.0 && !real_anchor) {
        // Backstop: the first real in-volume sample is already negative and the
        // only anchor is the slab entry boundary (entry epsilon is 1% of a voxel).
        // The entry point is itself inside geometry, so this is the hardware
        // backface case -- NOT a front hit. A genuine front surface at the boundary
        // reads near-zero POSITIVE at entry + 1% voxel, so this never swallows a
        // legitimate front hit; ordinary mid-march crossings (after a real positive
        // sample) still resolve as front hits via the bisection below.
        hit.inside = true;
        hit.miss = false;
        hit.hitT = 0.0;
        hit.pos = origin;
        return hit;
      }
      // A negative sample AFTER a proven-outside (real) anchor means the surface
      // lies between that anchor and here -- a front-surface crossing, never an
      // inside/backface (the origin already excluded that). Bisect toward the
      // crossing for a hit position/normal on the surface rather than inside it.
      if (d < 0.0 && real_anchor) {
        float lo = t_anchor, hi = t;
        [unroll]
        for (int b = 0; b < 4; ++b) {
          float tm = 0.5 * (lo + hi);
          float3 pm = origin + dir * tm;
          uint cm = SdfSelectClip(g, pm);
          if (cm >= kSdfClips) cm = c;
          if (SdfDistance(g, cm, pm, sdf_distance, sdf_sampler) < 0.0) hi = tm;
          else lo = tm;
        }
        t_hit = 0.5 * (lo + hi);
        p = origin + dir * t_hit;
        c = SdfSelectClip(g, p);
        if (c >= kSdfClips) c = kSdfClips - 1u;
        voxel = g.clip_origin[c].w;
      }
      hit.miss = false;
      hit.hitT = t_hit;
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
    // This sample was non-negative (d >= eps > 0), so it is a fresh, real
    // outside-geometry anchor for a later crossing's bisection.
    t_anchor = t;
    has_anchor = true;
    real_anchor = true;
    t += max(d, eps * 0.5);
  }
  return hit;
}

#endif  // RX_GI_SDF_TRACE_HLSLI_
