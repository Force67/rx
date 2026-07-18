#ifndef RX_PROCEDURAL_GRASS_HLSLI_
#define RX_PROCEDURAL_GRASS_HLSLI_

#include "rhi_bindings.hlsli"

[[vk::binding(0, 0)]] ByteAddressBuffer grass_instances : register(t0, space0);
[[vk::binding(1, 0)]] ByteAddressBuffer grass_interactions : register(t1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer grass_types : register(t2, space0);

struct GrassDrawPush {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  float4 camera_time;
  float4 sun_direction_intensity;
  float4 sun_color_ambient;
  float4 wind;       // speed, yaw, gustiness, delta time
  float4 jitter_lod; // jitter xy, geometry lod start/end
  uint4 control;     // interaction count, type count, vertices per blade
};
PUSH_CONSTANTS(GrassDrawPush, push);

struct GrassTypeData {
  float4 base_color;
  float4 tip_color;
  float4 dimensions;
  float4 shape;
  float4 material;
};

struct GrassInstance {
  float4 position_height;
  float4 facing_width_tint;
  float3 surface_normal;
  uint type;
  float4 shape_lod;
};

float3 SafeNormalize(float3 value, float3 fallback) {
  float length_squared = dot(value, value);
  return length_squared > 1e-8 ? value * rsqrt(length_squared) : fallback;
}

GrassTypeData LoadGrassType(uint type_index) {
  uint address = min(type_index, push.control.y - 1u) * 80u;
  GrassTypeData type;
  type.base_color = asfloat(grass_types.Load4(address + 0u));
  type.tip_color = asfloat(grass_types.Load4(address + 16u));
  type.dimensions = asfloat(grass_types.Load4(address + 32u));
  type.shape = asfloat(grass_types.Load4(address + 48u));
  type.material = asfloat(grass_types.Load4(address + 64u));
  return type;
}

GrassInstance LoadGrassInstance(uint instance_index) {
  uint address = instance_index * 64u;
  GrassInstance instance;
  instance.position_height = asfloat(grass_instances.Load4(address + 0u));
  instance.facing_width_tint = asfloat(grass_instances.Load4(address + 16u));
  uint4 surface_type = grass_instances.Load4(address + 32u);
  instance.surface_normal = asfloat(surface_type.xyz);
  instance.type = surface_type.w;
  instance.shape_lod = asfloat(grass_instances.Load4(address + 48u));
  return instance;
}

float3 TangentDirection(float2 xz, float3 surface_normal) {
  float3 direction = float3(xz.x, 0.0, xz.y);
  direction -= surface_normal * dot(direction, surface_normal);
  float len2 = dot(direction, direction);
  if (len2 < 1e-6) {
    float3 axis = abs(surface_normal.x) < 0.9 ? float3(1.0, 0.0, 0.0)
                                             : float3(0.0, 1.0, 0.0);
    direction = SafeNormalize(cross(surface_normal, axis), float3(0.0, 0.0, 1.0));
  }
  else direction *= rsqrt(len2);
  return direction;
}

float3 GrassDisplacement(GrassInstance instance, GrassTypeData type, float time) {
  float3 up = SafeNormalize(instance.surface_normal, float3(0.0, 1.0, 0.0));
  float3 wind_world = float3(cos(push.wind.y), 0.0, sin(push.wind.y));
  wind_world -= up * dot(wind_world, up);
  wind_world = SafeNormalize(wind_world, TangentDirection(float2(1.0, 0.0), up));
  float2 xz = instance.position_height.xz;
  float wave0 = sin(dot(xz, float2(0.105, 0.071)) + time * (0.7 + push.wind.x * 0.035));
  float wave1 = sin(dot(xz, float2(-0.047, 0.123)) - time * 1.37 + wave0 * 0.8);
  float gust = saturate(0.55 + wave0 * 0.25 + wave1 * push.wind.z * 0.35);
  float wind_strength = saturate(push.wind.x / 18.0) * type.material.y *
                        (0.08 + gust * 0.38);
  float3 displacement = wind_world * wind_strength;

  [loop]
  for (uint i = 0u; i < push.control.x; ++i) {
    uint address = i * 32u;
    float4 position_radius = asfloat(grass_interactions.Load4(address + 0u));
    float4 direction_strength = asfloat(grass_interactions.Load4(address + 16u));
    if (!all(isfinite(position_radius)) || !all(isfinite(direction_strength))) continue;
    float3 delta = instance.position_height.xyz - position_radius.xyz;
    float distance_to_source = length(delta);
    float influence = 1.0 - smoothstep(0.0, max(position_radius.w, 0.01),
                                       distance_to_source);
    if (influence <= 0.0) continue;
    float3 source_direction = direction_strength.xyz;
    float3 direction = source_direction - up * dot(source_direction, up);
    if (dot(source_direction, source_direction) < 1e-5) {
      direction = delta - up * dot(delta, up);
    }
    else if (dot(direction, direction) < 1e-5) continue;
    direction = SafeNormalize(direction, TangentDirection(instance.facing_width_tint.xy, up));
    displacement += direction * direction_strength.w * influence;
  }
  return displacement;
}

void EvaluateGrassCurve(GrassInstance instance, GrassTypeData type, float t,
                        float time, out float3 center, out float3 tangent) {
  float3 base = instance.position_height.xyz;
  float height = instance.position_height.w;
  float3 up = SafeNormalize(instance.surface_normal, float3(0.0, 1.0, 0.0));
  float3 bend_direction = TangentDirection(instance.facing_width_tint.xy, up);
  float3 width_direction = SafeNormalize(cross(up, bend_direction),
                                         TangentDirection(float2(0.0, 1.0), up));
  float3 displacement = GrassDisplacement(instance, type, time) * height;
  float3 tip_offset = bend_direction * (instance.shape_lod.x * height) + displacement;
  float3 p0 = base;
  float3 p1 = base + up * (height * 0.34) + bend_direction * (instance.shape_lod.y * height * 0.12);
  float3 p2 = base + up * (height * 0.72) + tip_offset * 0.48 +
              bend_direction * (instance.shape_lod.y * height * 0.28) +
              width_direction * (instance.shape_lod.z * height * 0.35);
  float3 p3 = base + up * height + tip_offset +
              width_direction * (instance.shape_lod.z * height);
  float omt = 1.0 - t;
  center = p0 * (omt * omt * omt) + p1 * (3.0 * omt * omt * t) +
           p2 * (3.0 * omt * t * t) + p3 * (t * t * t);
  tangent = (p1 - p0) * (3.0 * omt * omt) +
            (p2 - p1) * (6.0 * omt * t) + (p3 - p2) * (3.0 * t * t);
  // Small blade-local vertical phase breaks up perfectly coherent gust fronts.
  center += up * sin(time * 2.1 + instance.position_height.x * 1.7 +
                     instance.position_height.z * 1.13 + t * 2.4) *
            height * 0.018 * t * t;
}

struct GrassVsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 curr_clip : TEXCOORD1;
  [[vk::location(2)]] float4 prev_clip : TEXCOORD2;
  [[vk::location(3)]] float3 world_pos : TEXCOORD3;
  [[vk::location(4)]] float2 blade_uv : TEXCOORD0;
  [[vk::location(5)]] nointerpolation uint type : TEXCOORD4;
  [[vk::location(6)]] nointerpolation float tint : TEXCOORD5;
  [[vk::location(7)]] nointerpolation float lod : TEXCOORD6;
};

#endif  // RX_PROCEDURAL_GRASS_HLSLI_
