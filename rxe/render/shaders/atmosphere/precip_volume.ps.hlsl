#include "rhi_bindings.hlsli"
#include "precip_common.hlsli"
// Volumetric precipitation shading. Rain streaks are translucent water lit by
// transmittance-tinted sun/moon light with a strong forward-scattering lobe
// (looking toward the light brightens the sheet), plus sky ambient and a
// slightly blue lightning flash boost. Snow flakes are soft bright scatterers
// with a view-dependent glint twinkle. Both fade softly against the prepass
// depth and dim with the froxel fog transmittance, exactly like the billboard
// particles, so precipitation sits inside the scene's volumetrics.

PUSH_CONSTANTS(PrecipPush, push);

[[vk::binding(1, 0)]] Texture2D<float> scene_depth : register(t1, space0);  // reversed-z
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture3D<float4> froxel_volume : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState froxel_sampler : register(s2, space0);

// Matches the renderer's camera + froxel constants (near plane 0.1, froxel
// volume 0.1..64), pushed nowhere to keep the block inside 256 bytes.
static const float kNearPlane = 0.1;
static const float kFroxelNear = 0.1;
static const float kFroxelFar = 64.0;

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
  [[vk::location(1)]] float3 world_pos : TEXCOORD1;
  [[vk::location(2)]] float2 motion : TEXCOORD2;
  [[vk::location(3)]] float3 aux : TEXCOORD3;  // x sun vis, y rand, z width fade
};

struct PsOut {
  float4 color : SV_Target0;
  float4 motion : SV_Target1;  // xy velocity; w alpha for the blend
};

PsOut main(PsIn input) {
  bool snow = (push.flags & 1u) != 0u;
  float3 view = normalize(input.world_pos - push.cam_pos);
  float3 sun_dir = normalize(push.sun_dir);  // travel direction
  float3 sun = push.sun_color * push.sun_intensity * input.aux.x;
  // Sky ambient with a cool cast; precipitation never goes fully black at night.
  float3 sky_ambient = push.ambient * float3(0.75, 0.85, 1.1) * 2.0;
  // Lightning reads slightly blue and hits the whole sheet at once.
  float3 flash = push.wind.w * float3(0.55, 0.65, 1.0) * 1.6;

  float3 lit;
  float alpha;
  if (snow) {
    float r = length(input.uv);
    if (r > 1.0) discard;
    // Bright multiple-scattering flake body...
    lit = sun * 0.4 + sky_ambient * 1.6 + flash;
    // ...with a subtle view/sun-dependent glint: each flake carries a random
    // facet phase, so crystals twinkle as the camera or the flake moves.
    float facet = sin(push.time * (3.0 + input.aux.y * 6.0) + input.aux.y * 61.0 +
                      dot(view, float3(13.1, 17.7, 19.3)) * 24.0);
    lit += sun * smoothstep(0.90, 1.0, facet) * 2.0;
    alpha = 0.85 * smoothstep(1.0, 0.35, r);
  } else {
    // Translucent streak: mostly forward-scattered light. The lobe peaks when
    // looking toward the sun/moon through the rain.
    float fwd = pow(saturate(dot(view, sun_dir)), 6.0);
    lit = sun * (0.085 + 0.5 * fwd) + sky_ambient * 0.5 + flash * 0.6;
    // Thin core across the width, feathered ends along the length.
    float across = 1.0 - input.uv.x * input.uv.x;
    float along = 1.0 - input.uv.y * input.uv.y * input.uv.y * input.uv.y;
    alpha = 0.32 * across * along * (0.55 + 0.45 * input.aux.y);
  }
  // Refund of the minimum-pixel-size widening in the vertex stage.
  alpha *= input.aux.z;

  // Soft particles: fade against the opaque depth (linear view z), which also
  // hides drops behind geometry (no depth attachment is bound).
  uint w, h;
  scene_depth.GetDimensions(w, h);
  float scene_d = scene_depth.Load(int3(int2(input.pos.xy), 0)).r;
  float scene_vz = kNearPlane / max(scene_d, 1e-6);
  float part_vz = kNearPlane / max(input.pos.z, 1e-6);
  float soft = saturate((scene_vz - part_vz) / (snow ? 0.25 : 0.5));
  alpha *= soft;

  // Distance fade: drops at the wrap-volume rim dissolve instead of popping.
  float dist = length(input.world_pos - push.cam_pos);
  alpha *= saturate(1.6 - dist / (snow ? 12.0 : 20.0));
  if (alpha <= 0.002) discard;

  // Froxel transmittance dims the particle with the fog in front of it.
  if ((push.flags & 2u) != 0u) {
    float slice = saturate(log2(max(part_vz, kFroxelNear) / kFroxelNear) /
                           log2(kFroxelFar / kFroxelNear));
    float2 uv_screen = input.pos.xy / float2(w, h);
    lit *= froxel_volume.SampleLevel(froxel_sampler, float3(uv_screen, slice), 0.0).a;
  }

  PsOut o;
  o.color = float4(lit, alpha);
  o.motion = float4(input.motion, 0.0, alpha);  // alpha-weighted velocity
  return o;
}
