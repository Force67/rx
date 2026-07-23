// render2d 2D-lighting pass - vertex stage. One quad per light (its bounding
// box), instanced from SV_VertexID like the sprite pass. The fragment stage
// gets the interpolated world position plus the light's parameters and shapes
// the falloff; the pipeline blends the result additively into the light target.

#include "rhi_bindings.hlsli"

struct Light2D {
  float2 center;     // world position
  float  radius;     // reach, world units
  float  intensity;  // linear scale
  float4 color;      // linear rgb (a unused)
  float2 dir;        // spot facing (normalized); (0,0) = omni
  float  cone;       // cos(half-angle) for a spot, <=-1 for omni
  float  falloff;    // distance falloff exponent
};

struct Push {
  column_major float4x4 view_proj;
};
PUSH_CONSTANTS(Push, pc);

[[vk::binding(0, 0)]] StructuredBuffer<Light2D> lights : register(t0, space0);

static const float2 kCorner[6] = {
  float2(0, 0), float2(1, 0), float2(0, 1),
  float2(0, 1), float2(1, 0), float2(1, 1),
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 world : TEXCOORD0;
  [[vk::location(1)]] nointerpolation float2 center : TEXCOORD1;
  [[vk::location(2)]] nointerpolation float4 color : COLOR0;
  [[vk::location(3)]] nointerpolation float4 params : TEXCOORD2;  // radius, intensity, cone, falloff
  [[vk::location(4)]] nointerpolation float2 dir : TEXCOORD3;
};

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  Light2D l = lights[iid];
  float2 corner = kCorner[vid];
  float2 world = l.center + (corner * 2.0 - 1.0) * l.radius;
  float4 clip = mul(pc.view_proj, float4(world, 0.0, 1.0));
  clip.z = 0.0;

  VsOut o;
  o.pos = clip;
  o.world = world;
  o.center = l.center;
  o.color = l.color;
  o.params = float4(l.radius, l.intensity, l.cone, l.falloff);
  o.dir = l.dir;
  return o;
}
