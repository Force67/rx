#include "rhi_bindings.hlsli"
// Screen-space contact shadows: a short depth-buffer ray march toward the sun,
// folded into the SIGMA-denoised sun shadow in place. Catches the sub-30cm
// occlusion (feet, props, ledges) that a 1-spp denoised shadow ray blurs away,
// at grounding-detail scale only - the long-range shadow stays NRD's.
struct ContactPush {
  column_major float4x4 view_proj;      // unjittered
  column_major float4x4 inv_view_proj;  // unjittered
  float3 sun_dir;  // travel direction
  float near_plane;
  uint2 size;
  float range;   // world meters marched toward the sun
  float thickness;
  uint steps;
  uint frame_index;
  float pad0;
  float pad1;
};
PUSH_CONSTANTS(ContactPush, pc);

[[vk::image_format("r8")]] [[vk::binding(0, 0)]] RWTexture2D<float> sun_shadow : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);

float Ign(float2 p, uint frame) {
  p += float(frame & 63u) * 5.588238;
  return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  float depth = depth_map.Load(int3(id.xy, 0));
  if (depth <= 0.0) return;  // sky
  float existing = sun_shadow[id.xy];
  if (existing <= 0.01) return;  // already fully shadowed

  float2 uv = (float2(id.xy) + 0.5) / float2(pc.size);
  float2 ndc = uv * 2.0 - 1.0;
  float4 wh = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
  float3 world = wh.xyz / wh.w;
  float3 to_sun = normalize(-pc.sun_dir);

  float jitter = Ign(float2(id.xy), pc.frame_index);
  float occluded = 0.0;
  [loop]
  for (uint i = 0; i < pc.steps; ++i) {
    float t = (float(i) + jitter) / float(pc.steps) * pc.range;
    float3 p = world + to_sun * t;
    float4 clip = mul(pc.view_proj, float4(p, 1.0));
    if (clip.w <= 0.0) break;
    float3 sndc = clip.xyz / clip.w;
    float2 suv = sndc.xy * 0.5 + 0.5;
    if (any(suv < 0.0) || any(suv > 1.0)) break;
    float scene_d = depth_map.Load(int3(int2(suv * float2(pc.size)), 0));
    if (scene_d <= 0.0) continue;
    // Reversed infinite z: view z = near / ndc_z. The ray point must sit
    // BEHIND the depth buffer surface (further) by less than the thickness.
    float ray_z = pc.near_plane / max(sndc.z, 1e-6);
    float scene_z = pc.near_plane / scene_d;
    float diff = ray_z - scene_z;
    if (diff > 0.015 && diff < pc.thickness) {
      occluded = 1.0;
      break;
    }
  }
  if (occluded > 0.0) {
    sun_shadow[id.xy] = min(existing, 0.15);  // never full black: keep some bounce
  }
}
