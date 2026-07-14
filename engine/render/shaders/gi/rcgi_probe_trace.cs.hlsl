#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
// RCGI probe trace. For the current frame's cascade, every probe shoots a
// rotated fibonacci sphere of rays through the frame TLAS. Misses sample the
// sky straight into the rays buffer. Hits register/refresh a world-radiance-
// cache entry (packed triangle refs + world pos/normal, cell LOD by camera
// distance) and, when the entry is stale, append its slot to the active list
// for the shade pass. The rays buffer carries the signed hit distance the blend
// pass needs: w > 0 hit, w < 0 miss (rgb = sky), w == 0 backface.

#define RX_RCGI_ACTIVE_CAP (1u << 18)

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> rays_out : register(u0, space0);
[[vk::binding(1, 0)]] RaytracingAccelerationStructure tlas : register(t1, space0);
[[vk::binding(2, 0)]] ConstantBuffer<RcgiGlobals> rcgi : register(b2, space0);
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> rcgi_state_rw : register(u3, space0);
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> active_list : register(u4, space0);
[[vk::binding(5, 0)]] RWStructuredBuffer<uint> active_meta : register(u5, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] TextureCube sky : register(t6, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] SamplerState sky_sampler : register(s6, space0);

// Bindless scene tables (set 1) - just enough to fetch the hit normal.
#define RX_GEOMETRY_SPACE space1
#include "rt_geometry.hlsli"
[[vk::binding(0, 1)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space1);
[[vk::binding(1, 1)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space1);

struct PushData {
  float4 rotation_x;
  float4 rotation_y;
  float4 rotation_z;
};
PUSH_CONSTANTS(PushData, push);

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

  // Register/refresh the cache entry for this hit point.
  int3 q; uint lod_exp; float cell_size;
  RcgiCacheCell(rcgi, hit_pos, q, lod_exp, cell_size);
  uint checksum = RcgiCellChecksum(q, lod_exp);
  uint capacity = rcgi.misc.w;
  uint slot0 = RcgiCellHash(q, lod_exp) % capacity;
  uint stamp = RcgiStampEncode(frame);  // frame+1; 0 = never (matches cleared slot)

  // Locate (or claim) this cell's slot. Empty and matching slots take directly;
  // a slot held by a *different* cell that has not been queued for kRcgiEvictAge
  // frames is age-reclaimed (finding: no eviction) so stale cells cannot wedge
  // the probe chain forever.
  int found = -1;
  [loop]
  for (uint i = 0u; i < kRcgiHashProbe; ++i) {
    uint idx = (slot0 + i) % capacity;
    uint prev;
    InterlockedCompareExchange(rcgi_state_rw[idx * kRcgiEntry + kRcgiOffKey], 0u, checksum, prev);
    if (prev == 0u || prev == checksum) { found = int(idx); break; }
    // Occupied by another cell: reclaim if its last-queued stamp is stale enough.
    uint q_stamp = rcgi_state_rw[idx * kRcgiEntry + kRcgiOffQueued];
    if (q_stamp != 0u && (stamp - q_stamp) >= kRcgiEvictAge) {
      uint prev2;
      InterlockedCompareExchange(rcgi_state_rw[idx * kRcgiEntry + kRcgiOffKey], prev, checksum,
                                 prev2);
      if (prev2 == prev) {  // won the eviction race (ABA-tolerant: cache semantics)
        // Clear the shaded stamp so the previous occupant's radiance cannot leak
        // through RcgiCacheLookup before this cell is (re)shaded.
        rcgi_state_rw[idx * kRcgiEntry + kRcgiOffStamp] = 0u;
        found = int(idx);
        break;
      }
    }
  }
  if (found < 0) return;  // hash overflow: drop the sample

  uint base = uint(found) * kRcgiEntry;

  // Exactly one ray per cell per frame owns the multiword payload write. Claiming
  // it through the queued-frame stamp unifies the per-frame dedup with payload
  // ownership: losers write nothing, so the payload can never be torn across rays
  // (finding: torn cache payloads -> OOB geometry reads in the shade pass).
  uint prevq;
  InterlockedExchange(rcgi_state_rw[base + kRcgiOffQueued], stamp, prevq);
  if (prevq == stamp) return;  // another ray already owns this cell this frame

  rcgi_state_rw[base + kRcgiOffHit0] = (instance & 0x00ffffffu) | (geom_index << 24u);
  rcgi_state_rw[base + kRcgiOffHit1] = prim;
  rcgi_state_rw[base + kRcgiOffHit2] = f32tof16(bary.x) | (f32tof16(bary.y) << 16u);
  rcgi_state_rw[base + kRcgiOffPosX] = asuint(hit_pos.x);
  rcgi_state_rw[base + kRcgiOffPosY] = asuint(hit_pos.y);
  rcgi_state_rw[base + kRcgiOffPosZ] = asuint(hit_pos.z);
  rcgi_state_rw[base + kRcgiOffNrm] = RcgiPackOct(n);
  rcgi_state_rw[base + kRcgiOffHitT] = asuint(distance);

  // Re-shade round-robin: append to the active list only when the cached radiance
  // is stale (never shaded, or older than 4 frames). shaded stamp is +1 encoded.
  uint shaded = rcgi_state_rw[base + kRcgiOffStamp];
  bool stale = (shaded == 0u) || (stamp - shaded) >= 4u;
  if (stale) {
    uint slot;
    InterlockedAdd(active_meta[0], 1u, slot);
    if (slot < RX_RCGI_ACTIVE_CAP) active_list[slot] = uint(found);
  }
}
