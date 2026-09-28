#ifndef RX_PATH_MOTION_HLSLI_
#define RX_PATH_MOTION_HLSLI_

struct PathMotionRecord {
  column_major float4x4 previous_transform;
  uint previous_mesh;
  uint history_id;
  uint2 pad;
};

float2 PathMotion(float3 previous_position, float4x4 previous_view_proj,
                  float2 current_ndc, bool valid) {
  float4 clip = mul(previous_view_proj, float4(previous_position, 1.0));
  if (!valid || !(clip.w > 0.0) || !all(isfinite(clip))) return 2.0.xx;
  return clamp((clip.xy / clip.w - current_ndc) * 0.5, -2.0, 2.0);
}

#endif
