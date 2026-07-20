#include "procedural_grass.hlsli"

// Indexed ribbon: vertices 2r and 2r + 1 are the left/right edge of curve row
// r, shared between the segments above and below. Every tier draws a prefix of
// the same index pattern, so a blade costs (segments + 1) * 2 vertex
// invocations instead of segments * 6.

GrassVsOut main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
  GrassInstance instance = LoadGrassInstance(instance_id);
  GrassTypeData type = LoadGrassType(instance.type);
  uint row = vertex_id >> 1u;
  float side = (vertex_id & 1u) != 0u ? 1.0 : -1.0;
  float t = float(row) / float(push.control.z);

  float3 center, tangent;
  EvaluateGrassCurve(instance, type, t, push.camera_time.w,
                     instance.bend_history.xy, center, tangent);
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
  float pixel_scale = asfloat(push.control.x);
  if (pixel_scale > 0.0) {
    // Never let a ribbon collapse below roughly a pixel: distant blades stay
    // visible instead of dissolving into sub-pixel raster noise. Partial taper
    // keeps a silhouette on the widened blades.
    float center_depth = max(mul(push.view_proj, float4(center, 1.0)).w, 0.0);
    half_width = max(half_width, center_depth * pixel_scale * 0.5 *
                                     (0.35 + 0.65 * taper));
  }
  float3 world = center + width_direction * (side * half_width);
  float3 rounded_normal = SafeNormalize(
      plane_normal + width_direction * side * 0.28, plane_normal);

  float3 previous_center, previous_tangent;
  EvaluateGrassCurve(instance, type, t, push.camera_time.w - push.wind.w,
                     instance.bend_history.zw, previous_center,
                     previous_tangent);
  previous_tangent = SafeNormalize(previous_tangent, up);
  float3 previous_width = SafeNormalize(cross(previous_tangent, plane_normal), width_direction);
  float3 previous_world = previous_center + previous_width * (side * half_width);

  GrassVsOut output;
  output.curr_clip = mul(push.view_proj, float4(world, 1.0));
  output.prev_clip = mul(push.prev_view_proj, float4(previous_world, 1.0));
  output.sv_position = output.curr_clip;
  output.sv_position.xy += push.jitter_lod.xy * output.sv_position.w;
  output.normal = rounded_normal;
  output.world_pos = world;
  output.blade_uv = float2(t, side * 0.5 + 0.5);
  output.type = instance.type;
  output.tint = instance.facing_width_tint.w;
  output.lod = instance.shape_lod.w;
  return output;
}
