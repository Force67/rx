#include "rhi_bindings.hlsli"
#include "precip_common.hlsli"
// Rain impact splashes: stateless lifecycles in a disc around the camera.
// Every splash is two instances of one draw - a flat expanding ripple ring
// (even iid) and a tiny vertical crown flash (odd iid), both procedural in
// the pixel shader. The ground height comes from the sky-occlusion map's
// occluder height at the splash XZ, so roofs and bridge decks splash too;
// cells whose height is far outside the camera's vertical neighbourhood are
// skipped (their splash would be on an unrelated floor).

PUSH_CONSTANTS(PrecipPush, push);

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float> occlusion_map : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState occlusion_sampler : register(s0, space0);

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;      // -1..1 across the quad
  [[vk::location(1)]] float3 world_pos : TEXCOORD1;
  [[vk::location(2)]] float2 motion : TEXCOORD2;
  [[vk::location(3)]] float3 aux : TEXCOORD3;     // x life 0..1, y rand, z part (0 ring / 1 crown)
};

static const float kDiscSize = 16.0;   // splash field around the camera (m)
static const float kSplashRate = 2.5;  // 1 / 0.4 s lifetime

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  uint splash_id = iid >> 1u;
  bool crown = (iid & 1u) != 0u;
  float2 c = kQuadCorners[vid];

  // Stateless lifecycle: phase offset by hash, position re-seeded per cycle so
  // every 0.4 s the drop lands somewhere new.
  float4 h = Hash4(splash_id + 1u);
  float cycle_t = push.time * kSplashRate * (0.8 + 0.4 * h.x) + h.y;
  float life = frac(cycle_t);
  uint cycle = uint(cycle_t);
  float4 s = Hash4(splash_id * 977u + cycle * 613u + 1u);

  // World-anchored wrap into the camera window (fixed in world for its life).
  float3 p;
  p.x = WrapWindow(s.x * kDiscSize, push.cam_pos.x - kDiscSize * 0.5, kDiscSize);
  p.z = WrapWindow(s.y * kDiscSize, push.cam_pos.z - kDiscSize * 0.5, kDiscSize);

  // Land on whatever the sky-occlusion map says is the top surface here.
  float2 occ_uv = OcclusionUv(p.xz, push.occl);
  float occ_d = occlusion_map.SampleLevel(occlusion_sampler, occ_uv, 0.0);
  p.y = OccluderHeight(occ_d, push.occl, push.occl_range);

  VsOut o;
  o.uv = c;
  o.aux = float3(life, s.z, crown ? 1.0 : 0.0);

  // Density thinning by intensity (the full pool at once would carpet every
  // square metre in rings) and the vertical-neighbourhood cut: a height wildly
  // off the eye level means this cell's floor is not the one the player sees.
  bool dead = s.w > push.intensity * 0.3 || abs(p.y - push.cam_pos.y) > 14.0 ||
              occ_d >= 0.999;
  if (dead) {
    o.pos = float4(0.0, 0.0, -1.0, 1.0);
    o.world_pos = p;
    o.motion = float2(0.0, 0.0);
    return o;
  }

  float3 world;
  float base = 0.03 + 0.03 * s.z;  // 6-12 cm splashes
  if (crown) {
    // Tiny vertical camera-facing crown, only for the opening of the life.
    float hh = base * 0.5;
    world = p + push.cam_right * (c.x * base * 0.35) +
            float3(0.0, hh * (c.y * 0.5 + 0.5) + 0.002, 0.0);
  } else {
    // Flat expanding ring quad, slightly lifted to dodge z-fighting the floor.
    float half_size = base * (0.35 + 0.85 * life);
    world = p + float3(c.x * half_size, 0.004, c.y * half_size);
  }

  o.pos = mul(push.view_proj, float4(world, 1.0));
  o.pos.xy += push.jitter * o.pos.w;  // same temporal jitter as the geometry
  o.world_pos = world;
  // Splashes are world-static: their motion is pure camera reprojection.
  float4 cc = mul(push.view_proj, float4(p, 1.0));
  float4 pc = mul(push.prev_view_proj, float4(p, 1.0));
  o.motion = (pc.xy / pc.w - cc.xy / cc.w) * 0.5;
  return o;
}
