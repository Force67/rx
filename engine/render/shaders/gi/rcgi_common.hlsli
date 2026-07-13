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
static const uint kRcgiOffStamp = 9u;    // last shade frame (0 = never)
static const uint kRcgiOffQueued = 10u;  // last frame queued for shading
static const uint kRcgiOffPad = 11u;

static const float kRcgiPi = 3.14159265359;

struct RcgiGlobals {
  float4 cascade_origin[kRcgiCascades];  // xyz grid origin, w probe spacing
  float4 camera_pos;                     // xyz eye, w LOD distance (m)
  float4 sun_direction;                  // xyz travel dir, w intensity
  float4 sun_color;                      // rgb, w rays per probe
  uint4 counts;                          // probes x,y,z, irradiance texels
  uint4 misc;                            // x current cascade, y frame, z cascades, w hash capacity
  float4 params;                         // x max ray dist, y hysteresis, z energy scale, w base cell
};

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
                            Texture2D vis_atlas, SamplerState vis_smp, float3 world_pos,
                            float3 n, float3 v) {
  uint c;
  if (!RcgiSelectCascade(g, world_pos, c)) return 0.0.xxx;
  float spacing = g.cascade_origin[c].w;
  float3 origin = g.cascade_origin[c].xyz;

  float3 biased = world_pos + (n * 0.2 + v * 0.8) * spacing * 0.25;
  float3 local = clamp((biased - origin) / spacing, 0.0.xxx,
                       float(kRcgiProbesPerAxis - 1u).xxx - 0.001);
  uint3 base_probe = (uint3)local;
  float3 alpha = frac(local);

  float2 irr_size = RcgiAtlasSize(kRcgiIrrTexels);
  float2 vis_size = RcgiAtlasSize(kRcgiVisTexels);

  float3 sum = 0.0.xxx;
  float weight_sum = 0.0;
  [unroll]
  for (uint i = 0; i < 8; ++i) {
    uint3 off = uint3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
    uint3 probe = min(base_probe + off, (kRcgiProbesPerAxis - 1u).xxx);
    float3 probe_pos = origin + float3(probe) * spacing;

    float3 tri = lerp(1.0 - alpha, alpha, float3(off));
    float weight = tri.x * tri.y * tri.z;

    float3 to_probe = normalize(probe_pos - world_pos);
    float facing = (dot(to_probe, n) + 1.0) * 0.5;
    weight *= facing * facing + 0.2;

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
      rad = RcgiUnpackRadiance(radiance[idx]);
      return true;
    }
    if (key == 0u) return false;  // empty slot ends the probe chain
  }
  return false;
}

#endif  // RX_GI_RCGI_COMMON_HLSLI_
