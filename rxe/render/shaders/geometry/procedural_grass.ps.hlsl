#include "procedural_grass.hlsli"

struct PsOut {
  float4 color : SV_Target0;
  float2 motion : SV_Target1;
  float4 skin_diffuse : SV_Target2;
};

PsOut main(GrassVsOut input) {
  GrassTypeData type = LoadGrassType(input.type);
  float height_t = saturate(input.blade_uv.x);
  float3 albedo = lerp(type.base_color.rgb, type.tip_color.rgb,
                       smoothstep(0.0, 1.0, height_t));
  albedo *= input.tint;

  float3 normal = SafeNormalize(input.normal, float3(0.0, 1.0, 0.0));
  float3 light = SafeNormalize(-push.sun_direction_intensity.xyz,
                               float3(0.0, 1.0, 0.0));
  float ndl = saturate(dot(normal, light));
  float wrap = saturate((dot(normal, light) + 0.35) / 1.35);
  float backlight = saturate(dot(-normal, light)) * type.tip_color.a *
                    (0.25 + 0.75 * height_t);
  float root_ao = lerp(0.48, 1.0, smoothstep(0.0, 0.72, height_t));
  float3 diffuse = albedo * root_ao *
                   (push.sun_color_ambient.w +
                    push.sun_color_ambient.rgb * push.sun_direction_intensity.w *
                        (ndl * 0.18 + wrap * 0.08 + backlight * 0.16));

  // The tiny specular lobe is progressively removed as normals become
  // sub-pixel; this avoids wet-field glitter while retaining nearby sheen.
  float3 view = SafeNormalize(push.camera_time.xyz - input.world_pos, normal);
  float3 half_vector = SafeNormalize(light + view, normal);
  float roughness = lerp(type.material.x, 1.0, input.lod * 0.8);
  float gloss_power = lerp(96.0, 3.0, roughness);
  float specular = pow(saturate(dot(normal, half_vector)), gloss_power) *
                   (1.0 - input.lod) * 0.08 * ndl;

  PsOut output;
  output.color = float4(diffuse + push.sun_color_ambient.rgb * specular, 1.0);
  float2 current = input.curr_clip.xy / input.curr_clip.w;
  float2 previous = input.prev_clip.xy / input.prev_clip.w;
  output.motion = (previous - current) * 0.5;
  output.skin_diffuse = float4(0.0, 0.0, 0.0, 0.0);
  return output;
}
