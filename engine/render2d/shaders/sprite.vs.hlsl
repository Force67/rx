// render2d sprite pass - vertex stage. Six vertices per instance from
// SV_VertexID (two triangles, no vertex buffer); per-sprite transform, atlas uv
// rect and tint read from a StructuredBuffer indexed by SV_InstanceID.
// The camera hands its ortho view-proj through push constants; world space is
// y-down so +y is down on screen.

#include "rhi_bindings.hlsli"

struct SpriteInstance {
  float2 pos;       // top-left corner, world units
  float2 size;      // width, height, world units
  float2 uv_min;    // atlas uv of the top-left corner
  float2 uv_max;    // atlas uv of the bottom-right corner
  float4 color;     // linear rgba tint, multiplied into the texel
  float  rotation;  // radians, about the sprite centre
  float3 pad;
};

struct Push {
  column_major float4x4 view_proj;
};
PUSH_CONSTANTS(Push, pc);

[[vk::binding(0, 0)]] StructuredBuffer<SpriteInstance> instances : register(t0, space0);

static const float2 kCorner[6] = {
  float2(0, 0), float2(1, 0), float2(0, 1),
  float2(0, 1), float2(1, 0), float2(1, 1),
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float4 color : COLOR0;
};

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  SpriteInstance s = instances[iid];
  float2 corner = kCorner[vid];
  float2 local = (corner - 0.5) * s.size;
  float c = cos(s.rotation);
  float sn = sin(s.rotation);
  float2 rot = float2(local.x * c - local.y * sn, local.x * sn + local.y * c);
  float2 world = s.pos + s.size * 0.5 + rot;
  float4 clip = mul(pc.view_proj, float4(world, 0.0, 1.0));

  VsOut o;
  o.pos = clip;
  o.uv = lerp(s.uv_min, s.uv_max, corner);
  o.color = s.color;
  return o;
}
