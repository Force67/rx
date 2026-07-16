#ifndef RX_GI_RCGI_COMMON_HLSLI_
#define RX_GI_RCGI_COMMON_HLSLI_

// Shared interface for the RCGI world side (idTech8-style radiance-cached GI).
// M2 (final gather / denoise / upscale) consumes the functions here. All
// resources and the globals block are passed as parameters, so a pass includes
// this header first and then declares its own bindings (no fixed binding
// numbers baked in).
//
// The two M2-facing entry points:
//   SampleRcgiIrradiance(g, irr_atlas, irr_smp, vis_atlas, vis_smp, pos, n, v)
//     -> trilinear 8-probe, chebyshev-weighted irradiance in the smallest
//        cascade containing pos (adapts SampleDdgi).
//   RcgiCacheLookup(g, state, radiance, pos, out rad)
//     -> cached HDR radiance for the world cell at pos (LOD by camera distance);
//        false when the entry is absent/evicted.
//
// RcgiGlobals (bound by the includer as ConstantBuffer<RcgiGlobals>):
//   cascade_origin[c] = {origin.xyz, probe spacing}   per cascade
//   camera_pos        = {eye.xyz, LOD distance (m)}
//   sun_direction     = {travel dir.xyz, intensity}
//   sun_color         = {rgb, rays per probe}
//   counts            = {probesX, Y, Z, irradiance texels}
//   misc              = {current cascade, frame index, cascade count, hash capacity}
//   params            = {max ray distance, hysteresis, energy scale, base cell (m)}
//
// Buffer/texture contracts (see RcgiSystem in gi/rcgi.h for names/formats):
//   irradiance atlas : RGBA16F, perceptually (sqrt) encoded, 4 cascade slabs.
//   visibility atlas : RGBA16F, rg = hit-distance mean / mean^2, 4 slabs.
//   state buffer     : StructuredBuffer<uint>, kRcgiEntry u32 per hash slot.
//   radiance buffer  : StructuredBuffer<uint2>, packed RGBA16F per hash slot.

static const uint kRcgiProbesPerAxis = 16u;
static const uint kRcgiCascades = 4u;
static const uint kRcgiIrrTexels = 8u;   // interior irradiance texels / probe
static const uint kRcgiVisTexels = 8u;   // interior visibility texels / probe
static const uint kRcgiRaysPerProbe = 32u;
static const uint kRcgiEntry = 12u;      // u32 per hash slot (offsets below)
static const uint kRcgiHashProbe = 8u;   // linear-probe steps
// Age-based eviction: during insertion, a probed slot owned by a different cell
// whose last-queued stamp is older than this (frames) may be reclaimed. Cache
// semantics tolerate the ABA window this opens (a slot re-inserted within the
// same probe is rare and only costs one stale radiance sample).
static const uint kRcgiEvictAge = 256u;

// Hash-slot u32 offsets.
static const uint kRcgiOffKey = 0u;   // checksum, 0 = empty
static const uint kRcgiOffHit0 = 1u;  // instance(24) | geometry(8)
static const uint kRcgiOffHit1 = 2u;  // primitive id
static const uint kRcgiOffHit2 = 3u;  // bary.x f16 | bary.y f16
static const uint kRcgiOffPosX = 4u;  // world hit position (f32)
static const uint kRcgiOffPosY = 5u;
static const uint kRcgiOffPosZ = 6u;
static const uint kRcgiOffNrm = 7u;   // world normal, octahedral (2x f16)
static const uint kRcgiOffHitT = 8u;  // hit distance (f32)
static const uint kRcgiOffStamp = 9u;    // last shade frame, +1 encoded (0 = never shaded)
static const uint kRcgiOffQueued = 10u;  // last frame queued/owned, +1 encoded (0 = never)
static const uint kRcgiOffPad = 11u;

// Frame stamps stored in slots 9/10 are frame_index+1 so that a freshly cleared
// slot (0) reads as "never" and can never be mistaken for frame 0. Callers pass
// frame_index; encode/decode through these.
uint RcgiStampEncode(uint frame_index) { return frame_index + 1u; }

static const float kRcgiPi = 3.14159265359;

struct RcgiGlobals {
  float4 cascade_origin[kRcgiCascades];  // xyz grid origin, w probe spacing
  float4 camera_pos;                     // xyz eye, w LOD distance (m)
  float4 sun_direction;                  // xyz travel dir, w intensity
  float4 sun_color;                      // rgb, w rays per probe
  uint4 counts;                          // probes x,y,z, irradiance texels
  uint4 misc;                            // x current cascade, y frame, z cascades, w hash capacity
  float4 params;                         // x max ray dist, y hysteresis, z energy scale, w base cell
  uint4 valid;                           // x = per-cascade "blended since (re)creation" bitmask
  // ---- Phase 3 (leak & occlusion hardening) ----
  float4 interior;   // xyz interior ambient (ray-miss fallback when interior), w probe-AO scale
  uint4 gi_flags;    // x feature bits (below), y interior volume count, z asfloat probe-AO bias, w pad
};

// gi_flags.x feature bits. Each gated by its own env in the renderer.
static const uint kRcgiFlagInterior = 1u;  // ray misses fall back to interior ambient, not sky
static const uint kRcgiFlagRelocate = 2u;  // apply per-probe relocation offset from probe_meta
static const uint kRcgiFlagProbeAo = 4u;   // attenuate cascade-fallback by relative hit distance
static const uint kRcgiFlagClassify = 8u;  // downweight cross-class probes via interior volumes

bool RcgiInteriorMode(RcgiGlobals g) { return (g.gi_flags.x & kRcgiFlagInterior) != 0u; }
bool RcgiRelocateOn(RcgiGlobals g) { return (g.gi_flags.x & kRcgiFlagRelocate) != 0u; }
bool RcgiClassifyOn(RcgiGlobals g) { return (g.gi_flags.x & kRcgiFlagClassify) != 0u && g.gi_flags.y > 0u; }

// Ray-miss radiance for the visibility rays (probe trace) and gather rays: the
// sky cubemap outdoors, the authored interior ambient when interior mode is on
// (RX_RCGI_INTERIOR). This is the root skylight-leak fix -- an interior scene's
// probe cascades stop being fed sky through ceiling/doorway gaps.
float3 RcgiSkyMiss(RcgiGlobals g, float3 sky_radiance) {
  return RcgiInteriorMode(g) ? g.interior.xyz : sky_radiance;
}

// A cascade contributes only once it has actually been blended at least once
// since creation/teleport; until then its atlas slab is undefined (or freshly
// cleared to black) and must read as zero.
bool RcgiCascadeValid(RcgiGlobals g, uint cascade) {
  return (g.valid.x & (1u << cascade)) != 0u;
}

// ---- octahedral ----
float2 RcgiOctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}
float3 RcgiOctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 s = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * s;
  }
  return normalize(d);
}
uint RcgiPackOct(float3 n) {
  float2 o = RcgiOctEncode(n) * 0.5 + 0.5;
  return f32tof16(o.x) | (f32tof16(o.y) << 16u);
}
float3 RcgiUnpackOct(uint p) {
  float2 o = float2(f16tof32(p & 0xffffu), f16tof32(p >> 16u)) * 2.0 - 1.0;
  return RcgiOctDecode(o);
}

// ---- radiance packing (RGBA16F in uint2) ----
uint2 RcgiPackRadiance(float3 c) {
  c = max(c, 0.0.xxx);
  return uint2(f32tof16(c.r) | (f32tof16(c.g) << 16u), f32tof16(c.b));
}
float3 RcgiUnpackRadiance(uint2 p) {
  return float3(f16tof32(p.x & 0xffffu), f16tof32(p.x >> 16u), f16tof32(p.y & 0xffffu));
}

// ---- software (SDF-traced) cache entries ----
// The SDF probe trace has no instance/primitive/barycentric to re-resolve, so
// the software variant repurposes the triangle-reference slots (1..3, unused in
// software mode) to carry the surface colour the SDF gives it directly:
//   slot kRcgiOffHit0 (1) = albedo, 8-bit RGB packed in bits 0..23.
//   slot kRcgiOffHit1 (2) = emissive, 8-bit RGB packed in bits 0..23, with
//                           kRcgiSwEntryBit (bit 31) set to tag a software entry.
//   slot kRcgiOffHit2 (3) = free (left 0).
// World pos (4..6), oct normal (7), hitT (8), and the frame stamps (9/10) keep
// their hardware meaning. rcgi_cache_shade_sw reads albedo/emissive back from
// slots 1/2 instead of the bindless material path.
static const uint kRcgiSwEntryBit = 0x80000000u;
uint RcgiPackColor8(float3 c) {
  uint3 u = (uint3)round(saturate(c) * 255.0);
  return u.x | (u.y << 8u) | (u.z << 16u);
}
float3 RcgiUnpackColor8(uint p) {
  return float3(p & 0xffu, (p >> 8u) & 0xffu, (p >> 16u) & 0xffu) * (1.0 / 255.0);
}

// ---- fibonacci ray directions (rotated per frame like DDGI) ----
float3 RcgiFibonacci(uint i, uint n) {
  float phi = 2.0 * kRcgiPi * frac(i * 0.61803398875);
  float cos_theta = 1.0 - (2.0 * i + 1.0) / n;
  float sin_theta = sqrt(saturate(1.0 - cos_theta * cos_theta));
  return float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
}

// ---- probe / cascade geometry ----
uint3 RcgiProbeFromIndex(uint index) {
  uint px = index % kRcgiProbesPerAxis;
  uint py = (index / kRcgiProbesPerAxis) % kRcgiProbesPerAxis;
  uint pz = index / (kRcgiProbesPerAxis * kRcgiProbesPerAxis);
  return uint3(px, py, pz);
}
uint RcgiProbeIndex(uint3 p) {
  return p.x + p.y * kRcgiProbesPerAxis + p.z * kRcgiProbesPerAxis * kRcgiProbesPerAxis;
}
float3 RcgiProbePosition(RcgiGlobals g, uint cascade, uint3 probe) {
  return g.cascade_origin[cascade].xyz + float3(probe) * g.cascade_origin[cascade].w;
}

// ---- probe relocation metadata (Phase 3 item 10) ----
// Per (cascade, probe) uint2: .x = packed world-space offset (fraction of
// spacing), .y = flags (bit0 = disabled: drowning in backfaces even relocated).
static const uint kRcgiMetaDisabled = 1u;
static const float kRcgiRelocMaxOffset = 0.45;  // <= 0.45 cell (never leaves the cell)

uint RcgiMetaIndex(uint cascade, uint3 probe) {
  uint per_cascade = kRcgiProbesPerAxis * kRcgiProbesPerAxis * kRcgiProbesPerAxis;
  return cascade * per_cascade + RcgiProbeIndex(probe);
}
// Pack a per-axis offset (fraction of spacing, clamped to +-kRcgiRelocMaxOffset)
// into three 10-bit signed lanes. CPU mirror: render/gi/rcgi_interior.h.
uint RcgiPackOffset(float3 frac) {
  float3 n = clamp(frac / kRcgiRelocMaxOffset, -1.0.xxx, 1.0.xxx);
  uint3 q = (uint3)clamp(round(n * 511.0) + 512.0, 0.0.xxx, 1023.0.xxx);
  uint packed = q.x | (q.y << 10u) | (q.z << 20u);
  // Raw word 0 is reserved as the "no offset" sentinel (RcgiUnpackOffset below),
  // so a zero-cleared meta buffer reads as unrelocated rather than the maximum
  // negative corner. Nudge the all-min-negative offset (the only real value that
  // packs to 0) off the sentinel; the ~1/1022-cell shift is imperceptible.
  return packed == 0u ? 1u : packed;
}
float3 RcgiUnpackOffset(uint p) {
  if (p == 0u) return 0.0.xxx;  // zero-cleared meta == no offset (not (-0.45)^3)
  uint3 q = uint3(p & 1023u, (p >> 10u) & 1023u, (p >> 20u) & 1023u);
  float3 n = clamp((float3(q) - 512.0) / 511.0, -1.0.xxx, 1.0.xxx);
  return n * kRcgiRelocMaxOffset;  // fraction of spacing
}
// Relocated probe world position (base position when relocation is off). Used
// identically by the probe trace (ray origins), the blend (cache-key
// reconstruction) and the irradiance sample so their world positions agree.
float3 RcgiProbePositionMeta(RcgiGlobals g, StructuredBuffer<uint2> meta, uint cascade, uint3 probe) {
  float3 base = RcgiProbePosition(g, cascade, probe);
  if (!RcgiRelocateOn(g)) return base;
  return base + RcgiUnpackOffset(meta[RcgiMetaIndex(cascade, probe)].x) * g.cascade_origin[cascade].w;
}
bool RcgiProbeDisabledMeta(RcgiGlobals g, StructuredBuffer<uint2> meta, uint cascade, uint3 probe) {
  if (!RcgiRelocateOn(g)) return false;
  return (meta[RcgiMetaIndex(cascade, probe)].y & kRcgiMetaDisabled) != 0u;
}

// ---- interior-volume classification (Phase 3 item 9b) ----
// Volumes buffer: two float4 per box (min.xyz, max.xyz), g.gi_flags.y boxes.
bool RcgiPointInInterior(RcgiGlobals g, StructuredBuffer<float4> vols, float3 p) {
  uint n = min(g.gi_flags.y, 64u);
  [loop]
  for (uint i = 0u; i < n; ++i) {
    float3 lo = vols[i * 2u].xyz;
    float3 hi = vols[i * 2u + 1u].xyz;
    if (all(p >= lo) && all(p <= hi)) return true;
  }
  return false;
}

// Atlas dimensions: (texels+2)*probesX*probesZ wide; per-cascade slab
// (texels+2)*probesY tall; kRcgiCascades slabs stacked vertically.
float2 RcgiAtlasSize(uint texels) {
  float stride = float(texels) + 2.0;
  return float2(stride * kRcgiProbesPerAxis * kRcgiProbesPerAxis,
                stride * kRcgiProbesPerAxis * kRcgiCascades);
}
uint RcgiSlabHeight(uint texels) { return (texels + 2u) * kRcgiProbesPerAxis; }

// UV of the interior texel for `dir` at (cascade, probe) in the atlas of `texels`.
float2 RcgiAtlasUv(uint cascade, uint3 probe, float3 dir, uint texels, float2 atlas) {
  uint stride = texels + 2u;
  float2 oct = RcgiOctEncode(dir) * 0.5 + 0.5;
  float2 base = float2((probe.x + probe.z * kRcgiProbesPerAxis) * stride,
                       cascade * RcgiSlabHeight(texels) + probe.y * stride);
  base += 1.0;  // skip the border texel
  return (base + 0.5 + oct * (float(texels) - 1.0)) / atlas;
}

// Smallest cascade whose probe volume contains `pos`. false when outside all.
bool RcgiSelectCascade(RcgiGlobals g, float3 pos, out uint cascade) {
  cascade = 0u;
  for (uint c = 0u; c < g.misc.z; ++c) {
    float spacing = g.cascade_origin[c].w;
    float3 local = (pos - g.cascade_origin[c].xyz) / spacing;
    if (all(local >= 0.0) && all(local <= float(kRcgiProbesPerAxis - 1u))) {
      cascade = c;
      return true;
    }
  }
  return false;
}

// ---- world radiance cache: cell / hash ----
uint RcgiHashScalar(uint x) {
  x ^= x >> 17; x *= 0xed5ad4bbu;
  x ^= x >> 11; x *= 0xac4c1b51u;
  x ^= x >> 15; x *= 0x31848babu;
  x ^= x >> 14;
  return x;
}
uint RcgiCellHash(int3 q, uint lod_exp) {
  return RcgiHashScalar(lod_exp +
         RcgiHashScalar(uint(q.z) + RcgiHashScalar(uint(q.y) + RcgiHashScalar(uint(q.x)))));
}
uint RcgiCellChecksum(int3 q, uint lod_exp) {
  uint h = RcgiHashScalar(uint(q.x) * 73856093u ^ uint(q.y) * 19349663u ^
                          uint(q.z) * 83492791u ^ (lod_exp + 1u) * 2654435761u);
  return h | 1u;  // nonzero; 0 marks an empty slot
}
// Distance-based LOD from the camera. cellSize = base * 2^lod_exp.
void RcgiCacheCell(RcgiGlobals g, float3 pos, out int3 q, out uint lod_exp, out float cell_size) {
  float dist = length(pos - g.camera_pos.xyz);
  lod_exp = uint(floor(log2(1.0 + dist / g.camera_pos.w)));
  cell_size = g.params.w * exp2(float(lod_exp));
  q = int3(floor(pos / cell_size));
}

// Trilinear 8-probe blend with chebyshev visibility (the DDGI estimator) inside
// the smallest cascade containing `world_pos`. Returns the Lambert irradiance.
float3 SampleRcgiIrradiance(RcgiGlobals g, Texture2D irr_atlas, SamplerState irr_smp,
                            Texture2D vis_atlas, SamplerState vis_smp,
                            StructuredBuffer<uint2> meta, StructuredBuffer<float4> vols,
                            float3 world_pos, float3 n, float3 v) {
  uint c;
  if (!RcgiSelectCascade(g, world_pos, c)) return 0.0.xxx;
  // A not-yet-blended cascade holds undefined (or black) atlas data; skip it so
  // it contributes zero rather than garbage (finding: first-use blend).
  if (!RcgiCascadeValid(g, c)) return 0.0.xxx;
  float spacing = g.cascade_origin[c].w;
  float3 origin = g.cascade_origin[c].xyz;

  // Interior classification (item 9b): the sample's indoor/outdoor class; probes
  // of the opposite class are heavily downweighted so an outdoor probe cannot
  // bleed through a doorway onto an indoor surface (and vice versa).
  bool classify = RcgiClassifyOn(g);
  bool sample_indoor = classify ? RcgiPointInInterior(g, vols, world_pos) : false;

  float3 biased = world_pos + (n * 0.2 + v * 0.8) * spacing * 0.25;
  float3 local = clamp((biased - origin) / spacing, 0.0.xxx,
                       float(kRcgiProbesPerAxis - 1u).xxx - 0.001);
  uint3 base_probe = (uint3)local;
  float3 alpha = frac(local);

  float2 irr_size = RcgiAtlasSize(kRcgiIrrTexels);
  float2 vis_size = RcgiAtlasSize(kRcgiVisTexels);

  float3 sum = 0.0.xxx;
  float weight_sum = 0.0;
  // Weight from same-class probes only (before the cross-class attenuation). When
  // every nearby probe is the opposite class this stays ~0, which flags that the
  // renormalized result below is pure cross-wall bleed (finding: cross-class
  // rejection cancels under renormalization).
  float match_weight_sum = 0.0;
  [unroll]
  for (uint i = 0; i < 8; ++i) {
    uint3 off = uint3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
    uint3 probe = min(base_probe + off, (kRcgiProbesPerAxis - 1u).xxx);
    // Relocated probe position: keeps interpolation weights and the visibility
    // direction consistent with the (relocated) origin the probe traced from.
    float3 probe_pos = RcgiProbePositionMeta(g, meta, c, probe);

    float3 tri = lerp(1.0 - alpha, alpha, float3(off));
    float weight = tri.x * tri.y * tri.z;

    // A probe drowning in backfaces even after relocation contributes nothing.
    if (RcgiProbeDisabledMeta(g, meta, c, probe)) continue;
    // Cross-class probes bleed light across interior walls: near-zero weight.
    bool cross_class = classify && RcgiPointInInterior(g, vols, probe_pos) != sample_indoor;
    if (cross_class) weight *= 0.02;

    float3 to_probe = normalize(probe_pos - world_pos);
    float facing = (dot(to_probe, n) + 1.0) * 0.5;
    weight *= facing * facing + 0.2;
    if (!cross_class) match_weight_sum += weight;

    float3 from_probe = biased - probe_pos;
    float dist = length(from_probe);
    float2 moments = vis_atlas.SampleLevel(
        vis_smp, RcgiAtlasUv(c, probe, from_probe / max(dist, 1e-4), kRcgiVisTexels, vis_size),
        0.0).rg;
    if (dist > moments.x) {
      float variance = abs(moments.y - moments.x * moments.x);
      float diff = dist - moments.x;
      float vis = variance / (variance + diff * diff);
      weight *= max(vis * vis * vis, 0.05);
    }
    weight = max(weight, 1e-4);
    float3 irr = irr_atlas.SampleLevel(
        irr_smp, RcgiAtlasUv(c, probe, n, kRcgiIrrTexels, irr_size), 0.0).rgb;
    sum += irr * weight;  // atlas is perceptually (sqrt) encoded
    weight_sum += weight;
  }
  // No same-class probe contributed: the 8 taps are all the opposite class and
  // renormalizing their 0.02 weights would restore full cross-wall irradiance
  // (e.g. a small interior lit by outdoor probes only). Return no indirect --
  // conservative, and the sample's own class re-converges as its probes fill in.
  if (classify && match_weight_sum <= 1e-4) return 0.0.xxx;
  float3 mean = sum / max(weight_sum, 1e-4);
  return mean * mean * g.params.z;  // decode + energy scale
}

// Read-only world-radiance-cache probe.
bool RcgiCacheLookup(RcgiGlobals g, StructuredBuffer<uint> state, StructuredBuffer<uint2> radiance,
                     float3 world_pos, out float3 rad) {
  rad = 0.0.xxx;
  int3 q; uint lod_exp; float cell_size;
  RcgiCacheCell(g, world_pos, q, lod_exp, cell_size);
  uint checksum = RcgiCellChecksum(q, lod_exp);
  uint capacity = g.misc.w;
  uint slot0 = RcgiCellHash(q, lod_exp) % capacity;
  [loop]
  for (uint i = 0u; i < kRcgiHashProbe; ++i) {
    uint idx = (slot0 + i) % capacity;
    uint key = state[idx * kRcgiEntry + kRcgiOffKey];
    if (key == checksum) {
      // The key can be published (by the probe trace) a frame before the shade
      // pass fills the radiance, so an entry with a zero SHADED stamp still holds
      // undefined radiance. Reject it until it has been shaded at least once.
      if (state[idx * kRcgiEntry + kRcgiOffStamp] == 0u) return false;
      rad = RcgiUnpackRadiance(radiance[idx]);
      return true;
    }
    if (key == 0u) return false;  // empty slot ends the probe chain
  }
  return false;
}

#endif  // RX_GI_RCGI_COMMON_HLSLI_
