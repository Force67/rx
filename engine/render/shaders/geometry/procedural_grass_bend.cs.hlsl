#include "rhi_bindings.hlsli"

// Camera-following persistent grass deformation. RG carries the latest bend
// direction/magnitude; BA carries the previous-frame value for motion vectors.
struct PushData {
  float4 field;       // current min-corner xz, extent, inverse extent
  float4 prev_field;  // previous min-corner xz, extent, inverse extent
  float4 params;      // response dt, recovery elapsed, half-life, interaction count
  float4 height;      // current/previous height origin, history valid, unused
};
PUSH_CONSTANTS(PushData, push);

[[vk::combinedImageSampler]] [[vk::binding(0, 0)]]
Texture2D<float4> bend_previous : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]]
SamplerState bend_sampler : register(s0, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(1, 0)]]
RWTexture2D<float4> bend_current : register(u1, space0);
[[vk::binding(2, 0)]] ByteAddressBuffer interactions : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]]
Texture2D<float4> metadata_previous : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]]
SamplerState metadata_sampler : register(s3, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(4, 0)]]
RWTexture2D<float4> metadata_current : register(u4, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]]
Texture2D<float2> confidence_previous : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]]
SamplerState confidence_sampler : register(s5, space0);
[[vk::image_format("rg16f")]] [[vk::binding(6, 0)]]
RWTexture2D<float2> confidence_current : register(u6, space0);

static const uint kResolution = 512u;

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
  if (dispatch_id.x >= kResolution || dispatch_id.y >= kResolution) return;

  float2 uv = (float2(dispatch_id.xy) + 0.5) / float(kResolution);
  float2 world_xz = push.field.xy + uv * push.field.z;
  float2 previous_bend = 0.0;
  float2 previous_metadata = 0.0;
  float previous_confidence = 0.0;
  if (push.height.z > 0.5) {
    float2 prev_uv = (world_xz - push.prev_field.xy) * push.prev_field.w;
    if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0)) {
      previous_bend = bend_previous.SampleLevel(bend_sampler, prev_uv, 0.0).rg;
      previous_metadata =
          metadata_previous.SampleLevel(metadata_sampler, prev_uv, 0.0).rg;
      previous_confidence =
          confidence_previous.SampleLevel(confidence_sampler, prev_uv, 0.0).r;
      if (previous_confidence > 1e-4) {
        float absolute_height =
            push.height.y + previous_metadata.x / previous_confidence;
        float rebased_height = absolute_height - push.height.x;
        if (abs(rebased_height) * previous_confidence <= 60000.0) {
          previous_metadata.x = rebased_height * previous_confidence;
        } else {
          previous_bend = 0.0;
          previous_metadata = 0.0;
          previous_confidence = 0.0;
        }
      } else {
        previous_bend = 0.0;
        previous_metadata = 0.0;
        previous_confidence = 0.0;
      }
    }
  }

  float2 bend = previous_bend;
  float2 bend_metadata = previous_metadata;
  float confidence = previous_confidence;
  if (push.params.z > 0.0) {
    float decay = exp2(-push.params.y / push.params.z);
    bend *= decay;
    bend_metadata *= decay;
    confidence *= decay;
  }

  float best_stamp = 0.0;
  float2 stamp = 0.0;
  float2 stamp_metadata = 0.0;
  uint interaction_count = min(uint(push.params.w), 16u);
  [loop]
  for (uint i = 0u; i < interaction_count; ++i) {
    uint address = i * 32u;
    float4 position_radius = asfloat(interactions.Load4(address + 0u));
    float4 direction_strength = asfloat(interactions.Load4(address + 16u));
    if (!all(isfinite(position_radius)) || !all(isfinite(direction_strength))) continue;

    float texel_world = push.field.z / float(kResolution);
    float radius = clamp(max(position_radius.w, texel_world * 0.75), 0.01,
                         push.field.z);
    float2 delta = world_xz - position_radius.xz;
    float distance_to_source = length(delta);
    float influence = 1.0 - smoothstep(radius * 0.15, radius, distance_to_source);
    if (influence <= 0.0) continue;

    float2 direction = direction_strength.xz;
    if (dot(direction, direction) < 1e-5) {
      direction = delta;
    }
    float direction_length_squared = dot(direction, direction);
    if (direction_length_squared < 1e-6) direction = float2(1.0, 0.0);
    else direction *= rsqrt(direction_length_squared);

    float strength = clamp(direction_strength.w, -2.0, 2.0);
    float weight = abs(strength) * influence;
    float relative_height = position_radius.y - push.height.x;
    if (abs(relative_height) * weight > 60000.0 || radius * weight > 60000.0)
      continue;
    if (weight > best_stamp) {
      best_stamp = weight;
      stamp = direction * strength * influence;
      stamp_metadata = float2(relative_height, radius) * weight;
    }
  }

  if (best_stamp > 0.0) {
    bool compatible_layer = confidence <= 1e-4;
    if (!compatible_layer) {
      float old_height = push.height.x + bend_metadata.x / confidence;
      float old_radius = bend_metadata.y / confidence;
      float new_height = push.height.x + stamp_metadata.x / best_stamp;
      float new_radius = stamp_metadata.y / best_stamp;
      compatible_layer = abs(old_height - new_height) <= old_radius + new_radius;
    }
    if (!compatible_layer) {
      if (best_stamp >= confidence) {
        bend = stamp;
        bend_metadata = stamp_metadata;
        confidence = best_stamp;
      }
    } else {
      float response = 1.0 - exp(-push.params.x * 30.0 * saturate(best_stamp));
      bend = lerp(bend, stamp, response);
      bend_metadata = lerp(bend_metadata, stamp_metadata, response);
      confidence = lerp(confidence, best_stamp, response);
    }
  }
  float bend_length = length(bend);
  if (bend_length > 2.0) bend *= 2.0 / bend_length;
  bend_current[dispatch_id.xy] = float4(bend, previous_bend);
  metadata_current[dispatch_id.xy] = float4(bend_metadata, previous_metadata);
  confidence_current[dispatch_id.xy] =
      float2(confidence, previous_confidence);
}
