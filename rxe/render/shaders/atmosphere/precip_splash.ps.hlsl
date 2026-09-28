#include "rhi_bindings.hlsli"
#include "precip_common.hlsli"
// Splash shading: a thin expanding ripple ring (part 0) or a small vertical
// crown flash (part 1), both drawn as sky-tinted highlights that fade over the
// ~0.4 s life. Soft-faded against the prepass depth like every other
// translucent, so splashes behind walls disappear.

PUSH_CONSTANTS(PrecipPush, push);

[[vk::binding(1, 0)]] Texture2D<float> scene_depth : register(t1, space0);  // reversed-z

static const float kNearPlane = 0.1;

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float3 world_pos : TEXCOORD1;
  [[vk::location(2)]] float2 motion : TEXCOORD2;
  [[vk::location(3)]] float3 aux : TEXCOORD3;  // x life, y rand, z part
};

struct PsOut {
  float4 color : SV_Target0;
  float4 motion : SV_Target1;
};

PsOut main(PsIn input) {
  float life = input.aux.x;
  float mask;
  if (input.aux.z > 0.5) {
    // Crown: a short-lived vertical flick of spray right at impact.
    float burst = saturate(1.0 - life / 0.35);
    if (burst <= 0.0) discard;
    float across = saturate(1.0 - abs(input.uv.x) * 1.4);
    float up = saturate(1.0 - abs(input.uv.y - (life / 0.35) * 0.8));
    mask = across * up * burst * 0.9;
  } else {
    // Ring: a thin circle expanding through the quad, fading as it ages.
    float r = length(input.uv);
    float ring_r = 0.15 + 0.8 * life;
    float ring = exp(-pow((r - ring_r) * 9.0, 2.0));
    mask = ring * (1.0 - life) * 0.75;
  }

  // Splashes read as the sky/sun catching a wet film: mostly ambient with a
  // touch of sun and the lightning flash.
  float3 sun = push.sun_color * push.sun_intensity;
  float3 lit = push.ambient * float3(0.8, 0.9, 1.1) * 4.0 + sun * 0.30 +
               push.wind.w * float3(0.55, 0.65, 1.0);

  // Occlusion with tolerance instead of a soft fade: the splash sits ON the
  // surface it decorates (depths nearly equal), so a plain soft-particle ramp
  // would erase it. A small bias keeps it visible on its own floor while
  // geometry in front still hides it.
  float scene_d = scene_depth.Load(int3(int2(input.pos.xy), 0)).r;
  float scene_vz = kNearPlane / max(scene_d, 1e-6);
  float part_vz = kNearPlane / max(input.pos.z, 1e-6);
  float soft = saturate((scene_vz - part_vz + 0.06) / 0.06);

  float dist = length(input.world_pos - push.cam_pos);
  float alpha = mask * soft * saturate(1.4 - dist / 12.0) * saturate(push.intensity * 2.0);
  if (alpha <= 0.003) discard;

  PsOut o;
  o.color = float4(lit, alpha);
  o.motion = float4(input.motion, 0.0, alpha);
  return o;
}
