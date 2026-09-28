#include "rhi_bindings.hlsli"
#include "lightning_common.hlsli"
// Lightning bolt shading: a white-violet HDR core hot enough that the bloom
// chain flares it (same idea as the sky's sun disk kDiskBrightness) inside a
// wider faint blue-purple glow falloff, drawn additively over the lit scene.
// Depth-tested against the prepass depth so terrain occludes distant bolts.
// The froxel transmittance is deliberately NOT applied: the bolt IS the light
// source, and veiling it by its own scattered fog reads wrong - the fog in
// front instead brightens via the positioned flash light.

PUSH_CONSTANTS(LightningPush, push);

[[vk::binding(0, 0)]] Texture2D<float> scene_depth : register(t0, space0);  // reversed-z

static const float kNearPlane = 0.1;
static const float kCoreRadiance = 80.0;  // clips to white, drives the bloom
static const float kGlowRadiance = 10.0;

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float3 world_pos : TEXCOORD1;
  [[vk::location(2)]] float intensity : TEXCOORD2;
};

struct PsOut {
  float4 color : SV_Target0;   // additive (one, one); alpha unused
  float4 motion : SV_Target1;  // zero motion, alpha-weighted by the solid core
};

PsOut main(PsIn input) {
  float d = abs(input.uv.x);  // 0 at the channel centre .. 1 at the quad edge
  // Core occupies ~1/kGlowExtent of the quad; the glow decays across the rest.
  float core = exp(-d * d * 36.0);
  float glow = exp(-d * 4.0) * 0.10;

  // Depth test against the opaque scene (no depth attachment is bound): a
  // soft metre-scale fade hides the intersection line on terrain silhouettes.
  float scene_d = scene_depth.Load(int3(int2(input.pos.xy), 0)).r;
  float scene_vz = kNearPlane / max(scene_d, 1e-6);
  float bolt_vz = kNearPlane / max(input.pos.z, 1e-6);
  float vis = saturate((scene_vz - bolt_vz) / 4.0);

  float3 col = (float3(1.00, 0.96, 1.05) * (core * kCoreRadiance) +
                float3(0.50, 0.55, 1.00) * (glow * kGlowRadiance)) *
               (input.intensity * vis);
  if (max(col.r, max(col.g, col.b)) <= 0.002) discard;

  PsOut o;
  o.color = float4(col, 1.0);
  // The solid core writes zero motion so TAA does not drag sky/cloud velocity
  // through the flash; the faint glow leaves the background vectors alone.
  o.motion = float4(0.0, 0.0, 0.0, saturate(core * 2.0) * vis);
  return o;
}
