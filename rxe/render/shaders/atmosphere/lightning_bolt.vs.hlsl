#include "rhi_bindings.hlsli"
#include "lightning_common.hlsli"
// Procedural branched lightning bolt. Every instance is one camera-facing
// ribbon segment of the channel, generated statelessly from SV_InstanceID +
// the strike seed (the precip-volume philosophy: no CPU buffers). The main
// channel drops from the cloud base to strike_pos with midpoint-displacement
// jaggedness (7 octaves of endpoint-pinned value noise ~ 128 segments of
// detail); 3-5 side branches fork downward-outward at 40-60% of the channel
// length, thinner and dimmer. The stroke envelope modulates both brightness
// (in the PS, via the intensity interpolant) and ribbon width slightly
// (channel expansion between re-strokes).

PUSH_CONSTANTS(LightningPush, push);

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;        // x across -1..1, y along
  [[vk::location(1)]] float3 world_pos : TEXCOORD1;
  [[vk::location(2)]] float intensity : TEXCOORD2;  // envelope*energy*branch dim
};

static const uint kMainSegments = 128;
static const uint kBranchCount = 5;      // instance budget; 3-5 render
static const uint kBranchSegments = 24;
static const float kGlowExtent = 6.0;    // quad half width in core half widths

// Endpoint-pinned fractal offset: per octave, value noise minus the linear
// ramp of its own endpoint lattice values, so the sum is exactly zero at t=0
// and t=1 (the channel hits the cloud anchor and strike_pos precisely) -
// statistically the same jaggedness as midpoint displacement.
float2 Lattice(uint s, uint i) {
  return Hash4(s + i * 0x68bc21ebu).xy * 2.0 - 1.0;
}
float2 FractalOffset(float t, uint seed, uint octaves, float amp0) {
  float2 sum = float2(0.0, 0.0);
  float amp = amp0;
  for (uint l = 0; l < octaves; ++l) {
    uint cells = 2u << l;  // 2,4,...: the coarsest octave already bends the middle
    uint s = PcgHash(seed + l * 0x9e3779b9u);
    float x = t * cells;
    float i = floor(x);
    float f = x - i;
    f = f * f * (3.0 - 2.0 * f);
    float2 n = lerp(Lattice(s, (uint)i), Lattice(s, (uint)i + 1u), f);
    float2 ramp = lerp(Lattice(s, 0u), Lattice(s, cells), t);
    sum += (n - ramp) * amp;
    amp *= 0.55;
  }
  return sum;
}

// Main channel: cloud base 600-900 m up, horizontally offset 10-20% of the
// height at a hashed azimuth, down to strike_pos. t = 0 top, 1 ground.
float3 ChannelPoint(float t, float4 hs, float H) {
  float az = hs.y * kTau;
  float off = (0.10 + 0.10 * hs.z) * H;
  float3 top = push.strike_pos + float3(cos(az) * off, H, sin(az) * off);
  float3 p = lerp(top, push.strike_pos, t);
  p.xz += FractalOffset(t, push.seed ^ 0xa511e9b3u, 7u, 0.075 * H);
  return p;
}

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  VsOut o;
  o.uv = float2(0.0, 0.0);
  o.world_pos = float3(0.0, 0.0, 0.0);
  o.intensity = 0.0;
  o.pos = float4(0.0, 0.0, -1.0, 1.0);  // collapsed: clip-space z < 0 rejects

  float env = StrikeEnvelope(push.age, push.seed);
  if (env <= 0.005) return o;

  float4 hs = Hash4(push.seed * 2654435761u + 17u);
  float H = 600.0 + 300.0 * hs.x;

  // Segment endpoints along this instance's channel or branch.
  float3 a, b;
  float t_mid;       // 0 top .. 1 ground along the MAIN channel (width taper)
  float dim = 1.0;   // branch brightness
  float width_scale = 1.0;
  if (iid < kMainSegments) {
    float t0 = (float)iid / kMainSegments;
    float t1 = (float)(iid + 1u) / kMainSegments;
    a = ChannelPoint(t0, hs, H);
    b = ChannelPoint(t1, hs, H);
    t_mid = (t0 + t1) * 0.5;
  } else {
    uint bi = (iid - kMainSegments) / kBranchSegments;
    uint si = (iid - kMainSegments) % kBranchSegments;
    uint nb = 3u + (uint)(hs.w * 2.99);  // 3-5 branches per strike
    if (bi >= nb) return o;
    float4 hb = Hash4(push.seed * 747796405u + 101u + bi);
    float tb = 0.25 + 0.65 * hb.x;       // fork point on the main channel
    float3 origin = ChannelPoint(tb, hs, H);
    // Downward-outward: hashed azimuth, steeper than 45 degrees.
    float baz = hb.y * kTau;
    float3 bdir = normalize(float3(cos(baz), -(1.1 + 0.8 * hb.z), sin(baz)));
    // 40-60% of the channel length, clamped so tips stay above the ground.
    float blen = (0.40 + 0.20 * hb.w) * H * (1.0 - tb);
    blen = min(blen, (origin.y - 4.0) / -bdir.y);
    uint bseed = push.seed ^ (0x51ed270bu + bi * 0x9e3779b9u);
    float s0 = (float)si / kBranchSegments;
    float s1 = (float)(si + 1u) / kBranchSegments;
    a = origin + bdir * (s0 * blen);
    b = origin + bdir * (s1 * blen);
    a.xz += FractalOffset(s0, bseed, 5u, 0.09 * blen);
    b.xz += FractalOffset(s1, bseed, 5u, 0.09 * blen);
    t_mid = tb + s0 * (1.0 - tb);
    dim = 0.5;
    width_scale = 0.45;
  }

  // Camera-facing ribbon: core ~1.2 m wide near the ground, widening slightly
  // up the channel, breathing with the envelope (channel expansion); the quad
  // is kGlowExtent core widths wide so the PS glow falloff has room.
  float core_half = 0.5 * (1.2 + 1.0 * (1.0 - t_mid)) * width_scale;
  core_half *= 0.8 + 0.35 * saturate(env);
  float3 axis = normalize(b - a);
  // Slight overlap along the axis so adjacent segments join without cracks
  // (the overlap doubles additively - it reads as node glow, not a seam).
  float ext = length(b - a) * 0.06;
  float3 mid = (a + b) * 0.5;
  float3 side = cross(axis, mid - push.cam_pos);
  float side_len = length(side);
  if (side_len < 1e-4) return o;
  side /= side_len;

  float2 c = kQuadCorners[vid];
  float3 end = c.y < 0.0 ? a - axis * ext : b + axis * ext;
  float3 world = end + side * (c.x * core_half * kGlowExtent);

  o.uv = float2(c.x, c.y);
  o.world_pos = world;
  o.intensity = env * push.energy * dim;
  o.pos = mul(push.view_proj, float4(world, 1.0));
  // Rasterize with the frame's temporal jitter, like every geometry pass.
  o.pos.xy += push.jitter * o.pos.w;
  return o;
}
