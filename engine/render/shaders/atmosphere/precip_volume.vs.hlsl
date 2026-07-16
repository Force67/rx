#include "rhi_bindings.hlsli"
#include "precip_common.hlsli"
// Volumetric precipitation: stateless procedural GPU particles. Each instance
// is a rain streak or snow flake whose position is a pure function of
// (instance id, time, weather) inside a world-anchored wrap volume that
// follows the camera, so drops are fixed in world space and no simulation
// buffer exists. Rain draws as velocity-stretched cylindrical billboards
// (aligned to the fall+wind vector, camera-facing about that axis), snow as
// small camera-facing flakes with layered sway. The sky-occlusion map
// collapses particles below cover, so rain stops under roofs and bridges.
// RX_PRECIP_RT additionally shoots one inline ray toward the sun per vertex,
// darkening sheets of rain in shadowed alleys.

PUSH_CONSTANTS(PrecipPush, push);

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float> occlusion_map : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState occlusion_sampler : register(s0, space0);
#ifdef RX_PRECIP_RT
[[vk::binding(3, 0)]] RaytracingAccelerationStructure tlas : register(t3, space0);
#endif

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;       // -1..1 across the quad
  [[vk::location(1)]] float3 world_pos : TEXCOORD1;
  [[vk::location(2)]] float2 motion : TEXCOORD2;   // centre velocity, uv space
  [[vk::location(3)]] float3 aux : TEXCOORD3;      // x sun vis, y rand, z width fade
};

// Rain volume: ~30 m radius, 26 m tall. Snow reads best denser and closer
// (the same instance budget in a tighter volume = visible 3D depth).
static const float kRainW = 60.0, kRainH = 26.0;
static const float kSnowW = 26.0, kSnowH = 14.0;

// Particle centre at an arbitrary time, so the motion vector can evaluate the
// exact same function one frame earlier. h/g are the per-instance hashes.
float3 DropCenter(float t, float4 h, float4 g, bool snow, out float3 velocity) {
  float W = snow ? kSnowW : kRainW;
  float H = snow ? kSnowH : kRainH;
  // Gust wobble: low-frequency time noise scaled by gustiness; drops share the
  // wind but each carries its own phase so sheets billow instead of shifting
  // as one rigid block.
  float gust = sin(t * 0.7 + h.y * kTau) + 0.5 * sin(t * 2.1 + h.z * kTau);
  float2 wind_v = push.wind.xy * (1.0 + push.wind.z * gust * 0.4);
  float2 drift = push.wind.xy * t + push.wind.xy * (push.wind.z * gust * 0.6);

  float fall = snow ? (0.7 + 0.8 * h.x) : (9.0 + 4.0 * h.x);
  float2 sway = float2(0.0, 0.0);
  if (snow) {
    // Layered sinusoidal sway + a slow tumble drift; snow also rides the wind
    // harder than its fall speed, which sells the flurry look.
    sway.x = sin(t * (0.5 + h.z) + h.y * kTau) * (0.35 + 0.45 * g.x) +
             sin(t * 1.9 + g.y * kTau) * 0.12;
    sway.y = cos(t * (0.4 + g.z * 0.8) + h.w * kTau) * (0.3 + 0.4 * g.w) +
             cos(t * 2.3 + h.x * kTau) * 0.1;
    drift *= 0.8;
  }

  // World-anchored wrap: the drop's coordinate advances in world space and
  // only the visible window follows the camera, so drops hang still in the
  // world instead of gliding along with the eye.
  float3 p;
  p.x = WrapWindow(h.y * W + drift.x + sway.x, push.cam_pos.x - W * 0.5, W);
  p.z = WrapWindow(h.w * W + drift.y + sway.y, push.cam_pos.z - W * 0.5, W);
  // Vertical window is biased upward: most of the volume is above the eye,
  // where falling drops actually come from.
  p.y = WrapWindow(g.y * H - fall * t, push.cam_pos.y - H * 0.3, H);

  velocity = float3(wind_v.x * (snow ? 0.8 : 1.0), -fall, wind_v.y * (snow ? 0.8 : 1.0));
  return p;
}

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  float4 h = Hash4(iid + 1u);
  float4 g = Hash4(iid ^ 0x9e3779b9u);
  bool snow = (push.flags & 1u) != 0u;
  float2 c = kQuadCorners[vid];

  float3 vel;
  float3 center = DropCenter(push.time, h, g, snow, vel);

  VsOut o;
  o.uv = c;
  o.aux = float3(1.0, h.x, 1.0);

  // Sky occlusion: a particle below its column's occluder height is under
  // cover - collapse the quad (clip-space z < 0 rejects all four corners).
  float2 occ_uv = OcclusionUv(center.xz, push.occl);
  float occ_d = occlusion_map.SampleLevel(occlusion_sampler, occ_uv, 0.0);
  float occ_y = OccluderHeight(occ_d, push.occl, push.occl_range);
  if (center.y < occ_y - 0.5) {
    o.pos = float4(0.0, 0.0, -1.0, 1.0);
    o.world_pos = center;
    o.motion = float2(0.0, 0.0);
    return o;
  }

  // Sub-pixel particles do not survive the temporal resolve: clamp the drawn
  // size to roughly a pixel at this distance and refund the widening through
  // alpha (aux.z), so far precipitation reads as a dim haze instead of
  // flickering away.
  float dist = length(center - push.cam_pos);
  float pixel_size = dist * 0.0011;  // ~1 px at 1080p / 60 deg fov

  float3 world;
  if (snow) {
    // Soft round flakes, 4-14 mm, camera-facing.
    float half_size = 0.002 + 0.005 * g.x;
    float drawn = max(half_size, pixel_size);
    o.aux.z = half_size / drawn;
    world = center + push.cam_right * (c.x * drawn) + push.cam_up * (c.y * drawn);
  } else {
    // Velocity-stretched streak: a thin quad along the fall direction,
    // camera-facing about that axis (cylindrical billboard).
    float3 axis = normalize(vel);
    float3 to_cam = center - push.cam_pos;
    float3 side = cross(axis, to_cam);
    float side_len = length(side);
    side = side_len > 1e-4 ? side / side_len : push.cam_right;
    float half_len = clamp(length(vel) * 0.04, 0.18, 0.3);  // 0.36-0.6 m streaks
    float half_w = 0.0035;                                  // ~7 mm wide
    float drawn = max(half_w, pixel_size);
    o.aux.z = half_w / drawn;
    world = center + axis * (c.y * half_len) + side * (c.x * drawn);
  }

#ifdef RX_PRECIP_RT
  // One inline ray toward the sun per particle: rain sheets go dark in
  // shadowed alleys and sparkle in sunlit gaps. Realtime mask: fill geometry
  // that stays out of the TLAS also drops no visible shadow on the rain.
  {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> rq;
    RayDesc ray;
    ray.Origin = center;
    ray.Direction = -normalize(push.sun_dir);
    ray.TMin = 0.05;
    ray.TMax = 200.0;
    rq.TraceRayInline(tlas, 0, RX_RAY_MASK_REALTIME, ray);
    rq.Proceed();
    if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) o.aux.x = 0.15;
  }
#endif

  o.pos = mul(push.view_proj, float4(world, 1.0));
  // Rasterize with the frame's temporal jitter, like every geometry pass;
  // unjittered billboards swim against the jittered resolve (see particle.vs).
  o.pos.xy += push.jitter * o.pos.w;
  o.world_pos = world;

  // Exact centre motion: the same stateless function one frame earlier. A
  // drop that wrapped between the two evaluations would smear a huge vector
  // for one frame - detect the pop and zero it instead.
  float3 prev_vel;
  float3 prev_center = DropCenter(push.time - push.dt, h, g, snow, prev_vel);
  if (length(prev_center - center) > 2.0) prev_center = center;
  float4 cc = mul(push.view_proj, float4(center, 1.0));
  float4 pc = mul(push.prev_view_proj, float4(prev_center, 1.0));
  o.motion = (pc.xy / pc.w - cc.xy / cc.w) * 0.5;
  return o;
}
