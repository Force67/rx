#include "rhi_bindings.hlsli"
// Froxel volumetric lighting, pass 1: per-froxel inscatter. Every froxel gets
// height-fog density, then light from the sun (cascade-shadowed when the csm
// runs), the ambient term, and every clustered light in its cell - including
// their local shadow maps, which is the whole point: a lamp behind a wall
// must not light the fog in the next room. Temporally jittered inside the
// froxel and blended against last frame's reprojected volume.
struct ScatterPush {
  column_major float4x4 inv_view_proj;   // unjittered
  column_major float4x4 prev_view_proj;
  float4 camera_pos;      // xyz eye, w frame index
  float4 sun_dir_g;       // xyz travel dir, w henyey-greenstein g
  float4 sun_color;       // rgb * intensity, w ambient strength
  float4 density_params;  // x base density, y height falloff, z base height, w temporal alpha
  float4 volume_params;   // x near, y far, z slices, w csm active (0/1)
  float4 cluster_params;  // x slice scale, y slice bias, zw tile size px
  float4 screen_size;     // xy render size, z light count, w unused
};
PUSH_CONSTANTS(ScatterPush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture3D<float4> out_scatter : register(u0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture3D<float4> prev_scatter : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState prev_sampler : register(s1, space0);

struct PointLight {
  float4 pos_radius;
  float4 color_intensity;
  float4 direction_type;
  float4 params;
};
[[vk::binding(2, 0)]] StructuredBuffer<PointLight> lights : register(t2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint> cluster_counts : register(t3, space0);
[[vk::binding(4, 0)]] StructuredBuffer<uint> cluster_indices : register(t4, space0);

struct LocalShadowFace {
  column_major float4x4 view_proj;
  float4 rect;
};
[[vk::binding(5, 0)]] StructuredBuffer<LocalShadowFace> local_shadow_faces : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] Texture2D local_shadow_atlas : register(t6, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] SamplerComparisonState local_shadow_sampler : register(s6, space0);

struct CascadeData {
  column_major float4x4 light_view_proj[4];
  float4 p0;  // x count, y depth bias, z 1/count, w inset
  float4 p1;  // x cascade texel, z normal bias
};
[[vk::binding(7, 0)]] ConstantBuffer<CascadeData> cascades : register(b7, space0);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] Texture2D cascade_atlas : register(t8, space0);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] SamplerComparisonState cascade_sampler : register(s8, space0);

static const uint kClusterTilesX = 16;
static const uint kClusterTilesY = 9;
static const uint kClusterSlices = 24;
static const uint kMaxLightsPerCluster = 32;
static const float kPi = 3.14159265359;

float Halton(uint i, uint b) {
  float f = 1.0, r = 0.0;
  while (i > 0u) {
    f /= float(b);
    r += f * float(i % b);
    i /= b;
  }
  return r;
}

uint CubeFaceIndex(float3 d) {
  float3 a = abs(d);
  if (a.x >= a.y && a.x >= a.z) return d.x > 0.0 ? 0u : 1u;
  if (a.y >= a.z) return d.y > 0.0 ? 2u : 3u;
  return d.z > 0.0 ? 4u : 5u;
}

// Single-tap comparison against a local shadow face (the fog does not need
// the surface path's pcf).
float LocalShadow(uint face_index, float3 world_pos) {
  LocalShadowFace face = local_shadow_faces[face_index];
  float4 clip = mul(face.view_proj, float4(world_pos, 1.0));
  if (clip.w <= 0.0) return 1.0;
  float3 ndc = clip.xyz / clip.w;
  float2 uv = ndc.xy * 0.5 + 0.5;
  if (any(uv < 0.0) || any(uv > 1.0) || ndc.z <= 0.0 || ndc.z >= 1.0) return 1.0;
  const float inset = 1.5 / 512.0;
  uv = clamp(uv, inset, 1.0 - inset) * face.rect.xy + face.rect.zw;
  return local_shadow_atlas.SampleCmpLevelZero(local_shadow_sampler, uv, ndc.z - 0.002);
}

float CascadeShadow(float3 world_pos) {
  if (pc.volume_params.w < 0.5) return 1.0;
  uint count = (uint)cascades.p0.x;
  float inv_count = cascades.p0.z;
  float inset = cascades.p0.w;
  for (uint i = 0; i < count; ++i) {
    float4 clip = mul(cascades.light_view_proj[i], float4(world_pos, 1.0));
    float3 ndc = clip.xyz / clip.w;
    float2 uv = ndc.xy * 0.5 + 0.5;
    if (uv.x < inset || uv.x > 1.0 - inset || uv.y < inset || uv.y > 1.0 - inset) continue;
    if (ndc.z < 0.0 || ndc.z > 1.0) continue;
    float2 atlas_uv = float2((float(i) + uv.x) * inv_count, uv.y);
    return cascade_atlas.SampleCmpLevelZero(cascade_sampler, atlas_uv, ndc.z - cascades.p0.y);
  }
  return 1.0;
}

float PhaseHG(float cos_theta, float g) {
  float g2 = g * g;
  return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * cos_theta, 1e-3), 1.5));
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
  uint3 dims;
  out_scatter.GetDimensions(dims.x, dims.y, dims.z);
  if (any(id >= dims)) return;

  // Jittered position inside the froxel; the temporal blend integrates it.
  uint frame = uint(pc.camera_pos.w);
  uint seed = frame % 64u;
  float3 jitter = float3(Halton(seed + 1u, 2u), Halton(seed + 1u, 3u), Halton(seed + 1u, 5u));
  float2 uv = (float2(id.xy) + jitter.xy) / float2(dims.xy);
  float slice = (float(id.z) + jitter.z) / float(dims.z);
  float near = pc.volume_params.x;
  float far = pc.volume_params.y;
  float view_z = near * pow(far / near, slice);

  // Reversed-infinite projection: ndc depth = near / view_z.
  float2 ndc = uv * 2.0 - 1.0;
  float4 wh = mul(pc.inv_view_proj, float4(ndc, near / view_z, 1.0));
  float3 world = wh.xyz / wh.w;
  float3 view_dir = normalize(world - pc.camera_pos.xyz);

  // Height fog density.
  float density = pc.density_params.x *
                  exp(-pc.density_params.y * max(world.y - pc.density_params.z, 0.0));

  float3 inscatter = pc.sun_color.rgb * pc.sun_color.w;  // ambient term

  // Sun, cascade-shadowed on the raster tier.
  float3 to_sun = -pc.sun_dir_g.xyz;
  inscatter += pc.sun_color.rgb * PhaseHG(dot(view_dir, to_sun), pc.sun_dir_g.w) *
               CascadeShadow(world);

  // Clustered lights, with their local shadow maps.
  float2 px = uv * pc.screen_size.xy;
  uint tx = min(uint(px.x / pc.cluster_params.z), kClusterTilesX - 1u);
  uint ty = min(uint(px.y / pc.cluster_params.w), kClusterTilesY - 1u);
  uint tz = uint(clamp(log2(max(view_z, 1e-3)) * pc.cluster_params.x + pc.cluster_params.y, 0.0,
                       float(kClusterSlices - 1u)));
  uint cluster = (tz * kClusterTilesY + ty) * kClusterTilesX + tx;
  uint count = min(cluster_counts[cluster] & 0xffffu, kMaxLightsPerCluster);
  for (uint ci = 0; ci < count; ++ci) {
    PointLight pl = lights[cluster_indices[cluster * kMaxLightsPerCluster + ci]];
    float3 to_l = pl.pos_radius.xyz - world;
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
    uint shadow_face = uint(pl.params.w + 0.5);
    if (shadow_face != 0u && ltype <= 1u) {
      uint face = shadow_face - 1u + (ltype == 0u ? CubeFaceIndex(-pl_l) : 0u);
      falloff *= LocalShadow(face, world);
      if (falloff <= 0.0) continue;
    }
    // Isotropic phase for local lights.
    inscatter += pl.color_intensity.rgb * pl.color_intensity.w * falloff * (1.0 / (4.0 * kPi));
  }

  float4 result = float4(inscatter * density, density);

  // Temporal: reproject the froxel's world position into last frame's volume.
  float4 prev_clip = mul(pc.prev_view_proj, float4(world, 1.0));
  if (prev_clip.w > 0.0) {
    float3 prev_ndc = prev_clip.xyz / prev_clip.w;
    float2 prev_uv = prev_ndc.xy * 0.5 + 0.5;
    float prev_z = near / max(prev_ndc.z, 1e-6);  // reversed-infinite
    float prev_slice = log2(prev_z / near) / log2(far / near);
    if (all(prev_uv >= 0.0) && all(prev_uv <= 1.0) && prev_slice >= 0.0 && prev_slice <= 1.0) {
      float4 prev = prev_scatter.SampleLevel(prev_sampler, float3(prev_uv, prev_slice), 0.0);
      result = lerp(result, prev, pc.density_params.w);
    }
  }
  out_scatter[id] = result;
}
