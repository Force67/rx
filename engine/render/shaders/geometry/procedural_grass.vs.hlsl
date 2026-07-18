#include "procedural_grass.hlsli"

// Triangle-list corners for one strip segment: two triangles sharing their
// centerline rows. Seven segments always draw; far vertices collapse onto the
// same cubic curve's four-sample layout before rasterization.
static const uint kUpper[6] = {0u, 1u, 0u, 0u, 1u, 1u};
static const float kSide[6] = {-1.0, -1.0, 1.0, 1.0, -1.0, 1.0};

GrassVsOut main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
  GrassInstance instance = LoadGrassInstance(instance_id);
  GrassTypeData type = LoadGrassType(instance.type);
  uint segment = vertex_id / 6u;
  uint corner = vertex_id % 6u;
  uint row = segment + kUpper[corner];
  float t_high = float(row) / 7.0;
  float t_low = round(t_high * 3.0) / 3.0;
  float t = lerp(t_high, t_low, smoothstep(0.0, 1.0, instance.shape_lod.w));

  float3 center, tangent;
  EvaluateGrassCurve(instance, type, t, push.camera_time.w, center, tangent);
  float3 up = SafeNormalize(instance.surface_normal, float3(0.0, 1.0, 0.0));
  tangent = SafeNormalize(tangent, up);
  float3 plane_normal = TangentDirection(instance.facing_width_tint.xy, up);

  // A subtle camera-facing correction only when the ribbon is nearly edge-on.
  float3 to_camera = SafeNormalize(push.camera_time.xyz - center, plane_normal);
  float3 camera_on_surface = to_camera - up * dot(to_camera, up);
  if (dot(camera_on_surface, camera_on_surface) > 1e-5) {
    camera_on_surface = normalize(camera_on_surface);
    float edge_on = 1.0 - abs(dot(plane_normal, camera_on_surface));
    float sign_fix = dot(plane_normal, camera_on_surface) < 0.0 ? -1.0 : 1.0;
    plane_normal = normalize(lerp(plane_normal, camera_on_surface * sign_fix,
                                  edge_on * type.material.w));
  }
  float3 width_direction = SafeNormalize(cross(tangent, plane_normal),
                                         TangentDirection(float2(0.0, 1.0), up));
  float taper = pow(saturate(1.0 - t), 0.72);
  float half_width = instance.facing_width_tint.z * taper * 0.5;
  float3 world = center + width_direction * (kSide[corner] * half_width);
  float3 rounded_normal = SafeNormalize(
      plane_normal + width_direction * kSide[corner] * 0.28, plane_normal);

  float3 previous_center, previous_tangent;
  EvaluateGrassCurve(instance, type, t, push.camera_time.w - push.wind.w,
                     previous_center, previous_tangent);
  previous_tangent = SafeNormalize(previous_tangent, up);
  float3 previous_width = SafeNormalize(cross(previous_tangent, plane_normal), width_direction);
  float3 previous_world = previous_center + previous_width * (kSide[corner] * half_width);

  GrassVsOut output;
  output.curr_clip = mul(push.view_proj, float4(world, 1.0));
  output.prev_clip = mul(push.prev_view_proj, float4(previous_world, 1.0));
  output.sv_position = output.curr_clip;
  output.sv_position.xy += push.jitter_lod.xy * output.sv_position.w;
  output.normal = rounded_normal;
  output.world_pos = world;
  output.blade_uv = float2(t, kSide[corner] * 0.5 + 0.5);
  output.type = instance.type;
  output.tint = instance.facing_width_tint.w;
  output.lod = instance.shape_lod.w;
  return output;
}
