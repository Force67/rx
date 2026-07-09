#include "rhi_bindings.hlsli"
// Accumulates the weighted, premultiplied colour and the (1 - alpha) product for
// the WBOIT resolve. accum blends additively (src ONE, dst ONE); revealage
// blends (src ZERO, dst ONE_MINUS_SRC_COLOR) so it ends up as the transmittance.
struct PushData {
  column_major float4x4 view_proj;
  column_major float4x4 model;
  float4 color;
  float3 sun_dir;
  float pad0;
  float3 sun_color;
  float ambient;
  float4 cluster_params;  // x slice scale, y slice bias, zw tile size px
  float4 froxel_params;   // x near, y far, z enabled
};
PUSH_CONSTANTS(PushData, push);

struct PointLight {
  float4 pos_radius;
  float4 color_intensity;
  float4 direction_type;
  float4 params;
};
[[vk::binding(0, 0)]] StructuredBuffer<PointLight> lights : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<uint> cluster_counts : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint> cluster_indices : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture3D<float4> froxel_volume : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState froxel_sampler : register(s3, space0);

static const uint kClusterTilesX = 16;
static const uint kClusterTilesY = 9;
static const uint kClusterSlices = 24;
static const uint kMaxLightsPerCluster = 32;

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float view_z : TEXCOORD0;
  [[vk::location(2)]] float3 world_pos : TEXCOORD1;
};

struct PsOut {
  float4 accum : SV_Target0;
  float revealage : SV_Target1;
};

PsOut main(PsIn input) {
  float3 n = normalize(input.normal);
  float ndl = saturate(dot(n, -normalize(push.sun_dir)));
  float3 lit = push.color.rgb * (ndl * push.sun_color + push.ambient);
  float a = push.color.a;

  // Clustered lights: wrapped diffuse so glass picks up nearby torches.
  {
    uint tx = min(uint(input.pos.x / push.cluster_params.z), kClusterTilesX - 1u);
    uint ty = min(uint(input.pos.y / push.cluster_params.w), kClusterTilesY - 1u);
    uint tz = uint(clamp(log2(max(input.view_z, 1e-3)) * push.cluster_params.x +
                         push.cluster_params.y, 0.0, float(kClusterSlices - 1u)));
    uint cluster = (tz * kClusterTilesY + ty) * kClusterTilesX + tx;
    uint count = min(cluster_counts[cluster] & 0xffffu, kMaxLightsPerCluster);
    for (uint ci = 0; ci < count; ++ci) {
      PointLight pl = lights[cluster_indices[cluster * kMaxLightsPerCluster + ci]];
      float3 to_l = pl.pos_radius.xyz - input.world_pos;
      float dist2 = dot(to_l, to_l);
      float lr = pl.pos_radius.w;
      if (dist2 >= lr * lr) continue;
      float3 pl_l = to_l * rsqrt(max(dist2, 1e-8));
      uint ltype = uint(pl.direction_type.w + 0.5);
      float falloff = saturate(1.0 - dist2 / (lr * lr));
      falloff *= falloff;
      if (ltype == 1u) {
        float cd = dot(-pl_l, normalize(pl.direction_type.xyz));
        float att = saturate((cd - pl.params.y) / max(pl.params.x - pl.params.y, 1e-4));
        falloff *= att * att;
        if (falloff <= 0.0) continue;
      }
      float wrap = saturate((dot(n, pl_l) + 0.4) / 1.4);
      lit += push.color.rgb * pl.color_intensity.rgb * pl.color_intensity.w * falloff * wrap *
             0.25;
    }
  }

  // Froxel fog transmittance at this surface's depth.
  if (push.froxel_params.z > 0.5) {
    // Screen uv from the raster position; the render size is the cluster tile
    // size times the tile count, so no extra push field is needed.
    float2 uv = input.pos.xy / float2(push.cluster_params.z * kClusterTilesX,
                                      push.cluster_params.w * kClusterTilesY);
    float slice = saturate(log2(max(input.view_z, push.froxel_params.x) / push.froxel_params.x) /
                           log2(push.froxel_params.y / push.froxel_params.x));
    lit *= froxel_volume.SampleLevel(froxel_sampler, float3(uv, slice), 0.0).a;
  }

  // McGuire weight function: nearer, more opaque fragments contribute more.
  float z = input.view_z;
  float w = a * clamp(10.0 / (1e-5 + pow(z / 5.0, 2.0) + pow(z / 200.0, 6.0)), 1e-2, 3e3);

  PsOut o;
  o.accum = float4(lit * a, a) * w;
  o.revealage = a;
  return o;
}
