#include "rhi_bindings.hlsli"
// Authored environment map -> sky cubemap. Projects an equirectangular (latlong)
// HDR into the same cubemap the procedural atmosphere writes, so the existing
// irradiance and prefilter convolutions turn it into diffuse and specular IBL
// without knowing where the radiance came from. A UsdLux DomeLight is exactly
// this: an image that lights the scene from every direction.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2DArray<float4> sky_out : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D<float4> envmap : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState envmap_sampler : register(s1, space0);

struct PushData {
  float3 tint;      // DomeLight `color`, multiplies the map
  float face_size;
  float intensity;  // DomeLight `intensity` folded to engine scale
  float rotation;   // radians about up; domes are authored at arbitrary yaw
  float2 pad;
};
PUSH_CONSTANTS(PushData, push);

// Vulkan cubemap face order +x -x +y -y +z -z; uv in [0,1] within a face.
// Matches sky.cs.hlsl so both producers agree on orientation.
float3 CubeDir(uint face, float2 uv) {
  float2 c = uv * 2.0 - 1.0;
  float3 dir;
  if (face == 0) dir = float3(1.0, -c.y, -c.x);
  else if (face == 1) dir = float3(-1.0, -c.y, c.x);
  else if (face == 2) dir = float3(c.x, 1.0, c.y);
  else if (face == 3) dir = float3(c.x, -1.0, -c.y);
  else if (face == 4) dir = float3(c.x, -c.y, 1.0);
  else dir = float3(-c.x, -c.y, -1.0);
  return normalize(dir);
}

static const float kPi = 3.14159265358979;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint size = (uint)push.face_size;
  if (id.x >= size || id.y >= size) return;
  float3 dir = CubeDir(id.z, (float2(id.xy) + 0.5) / push.face_size);

  float c = cos(push.rotation), s = sin(push.rotation);
  float3 rotated = float3(c * dir.x - s * dir.z, dir.y, s * dir.x + c * dir.z);

  // Latlong: longitude across, latitude down, +y at v = 0.
  float2 uv;
  uv.x = atan2(rotated.x, -rotated.z) / (2.0 * kPi) + 0.5;
  uv.y = acos(clamp(rotated.y, -1.0, 1.0)) / kPi;

  float3 radiance = envmap.SampleLevel(envmap_sampler, uv, 0).rgb;
  sky_out[id] = float4(radiance * push.tint * push.intensity, 1.0);
}
