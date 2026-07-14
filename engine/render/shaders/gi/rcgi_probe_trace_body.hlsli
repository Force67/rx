#ifndef RX_GI_RCGI_PROBE_TRACE_BODY_HLSLI_
#define RX_GI_RCGI_PROBE_TRACE_BODY_HLSLI_

#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
// Shared body for the RCGI probe-trace pass. For the current frame's cascade,
// every probe shoots a rotated fibonacci sphere of visibility rays. Misses
// sample the sky straight into the rays buffer. Hits register/refresh a
// world-radiance-cache entry and, when stale, append its slot to the active
// list for the shade pass. The rays buffer carries the signed hit distance the
// blend pass needs: w > 0 hit, w < 0 miss (rgb = sky), w == 0 backface.
//
// The visibility trace is a seam: with RCGI_TRACE_SDF undefined it uses an
// inline TLAS RayQuery (hardware); with it defined it sphere-traces the global
// SDF clipmap (software, no RayQuery capability in the SPIR-V so the pipeline
// creates on non-ray-query devices). rcgi_probe_trace.cs.hlsl / _sw.cs.hlsl are
// the thin entry points selecting the variant.

#define RX_RCGI_ACTIVE_CAP (1u << 18)

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> rays_out : register(u0, space0);
#ifndef RCGI_TRACE_SDF
[[vk::binding(1, 0)]] RaytracingAccelerationStructure tlas : register(t1, space0);
#endif
[[vk::binding(2, 0)]] ConstantBuffer<RcgiGlobals> rcgi : register(b2, space0);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> rcgi_state_rw : register(u3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> active_list : register(u4, space0);
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> active_meta : register(u5, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] TextureCube sky : register(t6, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] SamplerState sky_sampler : register(s6, space0);

#ifndef RCGI_TRACE_SDF
// Bindless scene tables (set 1) - just enough to fetch the hit normal.
#define RX_GEOMETRY_SPACE space1
#include "rt_geometry.hlsli"
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);
#else
// SDF clipmap trace seam (set 1). No RayQuery, no bindless tables.
#include "gi/sdf_trace.hlsli"
[[vk::binding(0, 1)]] ConstantBuffer<SdfGlobals> sdf : register(b0, space1);
[[vk::binding(1, 1)]] Texture3D<float> sdf_distance : register(t1, space1);
[[vk::binding(2, 1)]] Texture3D<float4> sdf_albedo : register(t2, space1);
[[vk::binding(3, 1)]] Texture3D<float4> sdf_emissive : register(t3, space1);
[[vk::binding(4, 1)]] SamplerState sdf_sampler : register(s4, space1);
#endif

struct PushData {
  float4 rotation_x;
  float4 rotation_y;
  float4 rotation_z;
};
PUSH_CONSTANTS(PushData, push);

// Locate (or claim) this cell's slot for `hit_pos`; returns the slot or -1 on
// hash overflow. Shared by both variants (the per-slot payload differs, written
// by the caller). `stamp` is the RcgiStampEncode(frame) value (frame+1; 0=never).
//
// Empty and matching slots take directly; a slot held by a *different* cell that
// has not been queued for kRcgiEvictAge frames is age-reclaimed (finding: no
// eviction) so stale cells cannot wedge the probe chain forever. The eviction
// winner zeroes the shaded stamp so the previous occupant's radiance cannot leak
// through RcgiCacheLookup before this cell is (re)shaded.
int RcgiClaimCell(float3 hit_pos, uint stamp) {
  int3 q; uint lod_exp; float cell_size;
  RcgiCacheCell(rcgi, hit_pos, q, lod_exp, cell_size);
  uint checksum = RcgiCellChecksum(q, lod_exp);
  uint capacity = rcgi.misc.w;
  uint slot0 = RcgiCellHash(q, lod_exp) % capacity;
  [loop]
  for (uint i = 0u; i < kRcgiHashProbe; ++i) {
    uint idx = (slot0 + i) % capacity;
    uint prev;
    InterlockedCompareExchange(rcgi_state_rw[idx * kRcgiEntry + kRcgiOffKey], 0u, checksum, prev);
    if (prev == 0u || prev == checksum) return int(idx);
    // Occupied by another cell: reclaim if its last-queued stamp is stale enough.
    uint q_stamp = rcgi_state_rw[idx * kRcgiEntry + kRcgiOffQueued];
    if (q_stamp != 0u && (stamp - q_stamp) >= kRcgiEvictAge) {
      uint prev2;
      InterlockedCompareExchange(rcgi_state_rw[idx * kRcgiEntry + kRcgiOffKey], prev, checksum,
                                 prev2);
      if (prev2 == prev) {  // won the eviction race (ABA-tolerant: cache semantics)
        rcgi_state_rw[idx * kRcgiEntry + kRcgiOffStamp] = 0u;
        return int(idx);
      }
    }
  }
  return -1;  // hash overflow: drop the sample
}

// Re-shade round-robin: append the entry to the active list only when the cached
// radiance is stale (never shaded, or older than 4 frames). Called by the single
// per-cell payload owner (see main), so no per-frame dedup is needed here; the
// shaded stamp is +1 encoded, so `stamp` (also +1 encoded) is directly
// comparable.
void RcgiAppendIfStale(uint base, uint slot_index, uint stamp) {
  uint shaded = rcgi_state_rw[base + kRcgiOffStamp];
  bool stale = (shaded == 0u) || (stamp - shaded) >= 4u;
  if (stale) {
    uint slot;
    InterlockedAdd(active_meta[0], 1u, slot);
    if (slot < RX_RCGI_ACTIVE_CAP) active_list[slot] = slot_index;
  }
}

[numthreads(32, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint ray_count = (uint)rcgi.sun_color.w;
  uint probe_count = kRcgiProbesPerAxis * kRcgiProbesPerAxis * kRcgiProbesPerAxis;
  if (id.x >= ray_count || id.y >= probe_count) return;

  uint cascade = rcgi.misc.x;
  uint frame = rcgi.misc.y;
  uint3 probe = RcgiProbeFromIndex(id.y);
  float3 origin = RcgiProbePosition(rcgi, cascade, probe);
  float3 fib = RcgiFibonacci(id.x, ray_count);
  float3 dir = normalize(float3(dot(push.rotation_x.xyz, fib), dot(push.rotation_y.xyz, fib),
                                dot(push.rotation_z.xyz, fib)));
  uint stamp = RcgiStampEncode(frame);  // frame+1; 0 = never (matches cleared slot)

  // ---- visibility trace (variant): resolve hit_pos + normal (+ payload data) ----
#ifdef RCGI_TRACE_SDF
  // Software: sphere-trace the global SDF clipmap. Start bias = clip 0's voxel
  // (probes originate in free space / the finer clips).
  SdfHit sh = TraceGlobalSdf(origin, dir, rcgi.params.x, sdf.clip_origin[0].w, sdf, sdf_distance,
                             sdf_albedo, sdf_emissive, sdf_sampler);
  if (sh.miss) {
    float3 sky_rad = sky.SampleLevel(sky_sampler, dir, 0).rgb;
    rays_out[id.xy] = float4(sky_rad, -rcgi.params.x);  // miss
    return;
  }
  if (sh.inside) {
    // Ray started inside closed geometry: match the hardware backface case
    // (rays-buffer w == 0, no cache insertion; the blend pass reads it as a
    // visibility-only shortening ray).
    rays_out[id.xy] = float4(0, 0, 0, 0);
    return;
  }
  float distance = sh.hitT;
  float3 hit_pos = sh.pos;
  float3 n = sh.normal;
  if (dot(n, dir) > 0.0) n = -n;  // face the ray
  rays_out[id.xy] = float4(0, 0, 0, distance);  // hit
#else
  // Hardware: inline TLAS ray query.
  RayDesc ray;
  ray.Origin = origin;
  ray.TMin = 0.0;
  ray.Direction = dir;
  ray.TMax = rcgi.params.x;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, RX_RAY_MASK_REALTIME, ray);
  rq.Proceed();

  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    float3 sky_rad = sky.SampleLevel(sky_sampler, dir, 0).rgb;
    rays_out[id.xy] = float4(sky_rad, -rcgi.params.x);  // miss
    return;
  }

  float distance = rq.CommittedRayT();
  if (!rq.CommittedTriangleFrontFace()) {
    rays_out[id.xy] = float4(0, 0, 0, 0);  // backface: inside geometry
    return;
  }
  float3 hit_pos = origin + dir * distance;

  uint instance = rq.CommittedInstanceID();
  uint geom_index = rq.CommittedGeometryIndex();
  uint prim = rq.CommittedPrimitiveIndex();
  float2 bary = rq.CommittedTriangleBarycentrics();

  // Interpolated world normal (front-facing toward the ray).
  MeshRecord mesh = mesh_records[NonUniformResourceIndex(instance)];
  GeometryRecord geometry = geometry_records[mesh.geometry_offset + geom_index];
  uint3 tri = RxLoadTriangle(mesh, geometry.index_offset + prim * 3);
  float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
  float3 n_local = RxLoadNormal(mesh, tri[0]) * w[0] + RxLoadNormal(mesh, tri[1]) * w[1] +
                   RxLoadNormal(mesh, tri[2]) * w[2];
  float3x4 to_world = rq.CommittedObjectToWorld3x4();
  float3 n = normalize(mul((float3x3)to_world, n_local));
  if (dot(n, dir) > 0.0) n = -n;
  rays_out[id.xy] = float4(0, 0, 0, distance);  // hit; blend re-hashes for radiance
#endif

  // ---- shared cache insertion (identical for both variants) ----
  int found = RcgiClaimCell(hit_pos, stamp);
  if (found < 0) return;
  uint base = uint(found) * kRcgiEntry;

  // Exactly one ray per cell per frame owns the multiword payload write. Claiming
  // it through the queued-frame stamp unifies the per-frame dedup with payload
  // ownership: losers write nothing, so the payload can never be torn across rays
  // (finding: torn cache payloads -> OOB geometry reads in the shade pass). This
  // holds for the software payload too (albedo/emissive slots below).
  uint prevq;
  InterlockedExchange(rcgi_state_rw[base + kRcgiOffQueued], stamp, prevq);
  if (prevq == stamp) return;  // another ray already owns this cell this frame

  // Owner-only payload write (variant differs; only the triangle-ref slots).
#ifdef RCGI_TRACE_SDF
  // Software entry: pack the SDF surface colour into the triangle-ref slots.
  rcgi_state_rw[base + kRcgiOffHit0] = RcgiPackColor8(sh.albedo);
  rcgi_state_rw[base + kRcgiOffHit1] = RcgiPackColor8(sh.emissive) | kRcgiSwEntryBit;
  rcgi_state_rw[base + kRcgiOffHit2] = 0u;
#else
  rcgi_state_rw[base + kRcgiOffHit0] = (instance & 0x00ffffffu) | (geom_index << 24u);
  rcgi_state_rw[base + kRcgiOffHit1] = prim;
  rcgi_state_rw[base + kRcgiOffHit2] = f32tof16(bary.x) | (f32tof16(bary.y) << 16u);
#endif
  rcgi_state_rw[base + kRcgiOffPosX] = asuint(hit_pos.x);
  rcgi_state_rw[base + kRcgiOffPosY] = asuint(hit_pos.y);
  rcgi_state_rw[base + kRcgiOffPosZ] = asuint(hit_pos.z);
  rcgi_state_rw[base + kRcgiOffNrm] = RcgiPackOct(n);
  rcgi_state_rw[base + kRcgiOffHitT] = asuint(distance);

  RcgiAppendIfStale(base, uint(found), stamp);
}

#endif  // RX_GI_RCGI_PROBE_TRACE_BODY_HLSLI_
