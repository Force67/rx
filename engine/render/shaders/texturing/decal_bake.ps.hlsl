// Composites this draw's run of decal stamps into one premultiplied result and
// lets the fixed-function blend put it over the receiver's tile. Looping in the
// shader (rather than one draw per stamp) means a rebake of a receiver's whole
// journal after an eviction costs exactly one draw.
//
// Targets, mirroring DecalBaker's atlases:
//   0  albedo  premultiplied decal colour, a = coverage      (premultiplied blend)
//   1  fx      premultiplied normal xy + roughness, a = cov  (premultiplied blend)
//   2  chart   1 wherever the receiver's geometry rasterizes (no blend)
// The chart target is written unconditionally, including where no stamp lands:
// it is the UV-chart mask the gutter fill needs, and every bake draw covers the
// whole mesh anyway.

#include "rhi_bindings.hlsli"

struct BakePush {
  column_major float4x4 model;
  uint first_stamp;
  uint stamp_count;
  uint skin_offset;
  uint pad;
  float2 uv_scale;
  float2 uv_bias;
};
PUSH_CONSTANTS(BakePush, push);

// One stamp: the same oriented-box projector the clustered path uses.
struct Stamp {
  float4 row0;        // world -> unit box rows (xyz in [-1,1], z along the normal)
  float4 row1;
  float4 row2;
  float4 uv_rect;     // source atlas scale.xy, offset.zw
  float4 tint_blend;  // rgb tint, w opacity
  float4 params2;     // x normal strength, y roughness multiplier, zw unused
};
[[vk::binding(0, 0)]] StructuredBuffer<Stamp> stamps : register(t0, space0);
// The authored decal atlas (Renderer::SetDecalAtlas), shared with the clustered
// projector path: colour+alpha, and a normal in the projector's box basis.
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D source_albedo : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState source_albedo_sampler : register(s2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D source_normal : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState source_normal_sampler : register(s3, space0);

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 world_pos : TEXCOORD0;
  [[vk::location(1)]] float3 normal : NORMAL;
  [[vk::location(2)]] float4 tangent : TANGENT;
};

struct PsOut {
  float4 albedo : SV_Target0;
  float4 fx : SV_Target1;
  float chart : SV_Target2;
};

PsOut main(PsIn input) {
  float3 n = normalize(input.normal);
  // Receiver tangent frame, so the baked normal detail lands in the same space
  // as the material's normal map and the two just add in the forward pass.
  float3 t = input.tangent.xyz - n * dot(n, input.tangent.xyz);
  float t_len = length(t);
  t = t_len > 1e-5 ? t / t_len : float3(1, 0, 0);
  float3 b = cross(n, t) * (input.tangent.w != 0.0 ? input.tangent.w : 1.0);

  // Screen-space derivatives of the world position are taken ONCE, outside the
  // loop: per-stamp atlas derivatives follow analytically from the projector
  // rows, which keeps every fetch a SampleGrad in uniform control flow.
  float3 dwdx = ddx(input.world_pos);
  float3 dwdy = ddy(input.world_pos);

  float3 color_pre = float3(0, 0, 0);
  float3 fx_pre = float3(0, 0, 0);
  float coverage = 0.0;

  for (uint i = 0; i < push.stamp_count; ++i) {
    Stamp s = stamps[push.first_stamp + i];
    float3 local = float3(dot(s.row0.xyz, input.world_pos) + s.row0.w,
                          dot(s.row1.xyz, input.world_pos) + s.row1.w,
                          dot(s.row2.xyz, input.world_pos) + s.row2.w);
    if (any(abs(local) > 1.0)) continue;
    // Reject back faces and surfaces too oblique to the projector, then fade
    // the box rim so a splat has soft edges instead of a stencilled cut.
    float3 proj_dir = normalize(s.row2.xyz);
    float facing = dot(n, proj_dir);
    if (facing < 0.25) continue;

    float2 uv = local.xy * 0.5 + 0.5;
    float2 atlas_uv = uv * s.uv_rect.xy + s.uv_rect.zw;
    float2 duv_dx = float2(dot(s.row0.xyz, dwdx), dot(s.row1.xyz, dwdx)) * 0.5 * s.uv_rect.xy;
    float2 duv_dy = float2(dot(s.row0.xyz, dwdy), dot(s.row1.xyz, dwdy)) * 0.5 * s.uv_rect.xy;
    float4 source = source_albedo.SampleGrad(source_albedo_sampler, atlas_uv, duv_dx, duv_dy);

    float edge = saturate((1.0 - max(abs(local.x), abs(local.y))) * 6.0) *
                 saturate((1.0 - abs(local.z)) * 3.0);
    float w = saturate(source.a * s.tint_blend.w * edge * saturate((facing - 0.25) / 0.5));
    if (w <= 0.001) continue;

    float3 color = source.rgb * s.tint_blend.rgb;
    // fx.xy: the decal's normal detail rotated out of the projector basis and
    // into the receiver's tangent frame. fx.z: the roughness multiplier, half
    // scaled so 1 (unchanged) is the neutral 0.5 the tile clears to.
    float2 detail = float2(0, 0);
    if (s.params2.x > 0.001) {
      float3 tn = source_normal.SampleGrad(source_normal_sampler, atlas_uv, duv_dx, duv_dy).rgb *
                      2.0 - 1.0;
      float3 world_n = normalize(tn.x * normalize(s.row0.xyz) + tn.y * normalize(s.row1.xyz) +
                                 tn.z * proj_dir);
      detail = float2(dot(world_n, t), dot(world_n, b)) * s.params2.x;
    }
    float3 fx_value = float3(detail * 0.5 + 0.5, saturate(s.params2.y * 0.5));

    color_pre = color * w + color_pre * (1.0 - w);
    fx_pre = fx_value * w + fx_pre * (1.0 - w);
    coverage = w + coverage * (1.0 - w);
  }

  PsOut output;
  output.albedo = float4(color_pre, coverage);
  output.fx = float4(fx_pre, coverage);
  output.chart = 1.0;
  return output;
}
