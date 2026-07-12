#include "rhi_bindings.hlsli"
#include "vgeo_common.hlsli"
// Fullscreen resolve of the visibility buffer: decodes the packed
// cluster/triangle id, refetches the three vertices, reconstructs
// perspective-correct barycentrics from the pixel's NDC position and shades.
// Depth comes straight from the packed value and exports through SV_Depth, so
// the pass composites against the regular scene with the normal reversed-z
// test (pixels the scene already covers closer simply fail it).

[[vk::binding(0, 0)]] StructuredBuffer<VgeoParams> params_buf : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<DagMeshlet> meshlets : register(t1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint> meshlet_vertices : register(t2, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint> meshlet_triangles : register(t3, space0);
[[vk::binding(4, 0)]] StructuredBuffer<MVertex> vertices : register(t4, space0);
[[vk::binding(5, 0)]] StructuredBuffer<VgeoInstance> instances : register(t5, space0);
[[vk::binding(6, 0)]] StructuredBuffer<uint2> visible : register(t6, space0);
[[vk::binding(7, 0)]] StructuredBuffer<uint64_t> visbuffer : register(t7, space0);
// Planar-projected albedo (uv = world.xz * hiz.w + 0.5; hiz.w = 0 disables).
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] Texture2D albedo : register(t8, space0);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] SamplerState albedo_sampler : register(s8, space0);

struct PsOut {
  float4 color : SV_Target0;
  float depth : SV_Depth;
};

PsOut main(float4 pos : SV_Position) {
  VgeoParams p = params_buf[0];
  uint2 px = uint2(pos.xy);
  if (px.x >= p.width || px.y >= p.height) discard;  // dynamic-res guard band
  uint64_t value = visbuffer[px.y * p.width + px.x];
  uint payload = uint(value);
  if ((payload & ~VGEO_PAYLOAD_SW) == 0) discard;

  bool sw = (payload & VGEO_PAYLOAD_SW) != 0;
  uint tri = payload & 0x7fu;
  uint vi = ((payload & ~VGEO_PAYLOAD_SW) >> 7) - 1u;
  uint2 entry = visible[vi];
  DagMeshlet m = meshlets[entry.x];
  VgeoInstance inst = instances[entry.y];

  uint packed = meshlet_triangles[m.triangle_offset + tri];
  float3 n[3];
  float3 wp[3];
  float3 ndc[3];
  float inv_w[3];
  [unroll]
  for (uint k = 0; k < 3; ++k) {
    MVertex mv = vertices[meshlet_vertices[m.vertex_offset + ((packed >> (8 * k)) & 0xffu)]];
    float4 world = mul(inst.model, float4(mv.px, mv.py, mv.pz, 1.0));
    float4 clip = mul(p.view_proj, world);
    float w = max(abs(clip.w), 1e-6) * sign(clip.w == 0.0 ? 1.0 : clip.w);
    inv_w[k] = 1.0 / w;
    ndc[k] = float3(clip.xy * inv_w[k], clip.z * inv_w[k]);
    wp[k] = world.xyz;
    n[k] = mul((float3x3)inst.model, float3(mv.nx, mv.ny, mv.nz));
  }

  float2 s;
  s.x = (pos.x / p.width) * 2.0 - 1.0;
  s.y = (pos.y / p.height) * 2.0 - 1.0;
  // Screen-space sub-areas -> perspective-correct weights via 1/w.
  float a0 = (ndc[1].x - s.x) * (ndc[2].y - s.y) - (ndc[1].y - s.y) * (ndc[2].x - s.x);
  float a1 = (ndc[2].x - s.x) * (ndc[0].y - s.y) - (ndc[2].y - s.y) * (ndc[0].x - s.x);
  float a2 = (ndc[0].x - s.x) * (ndc[1].y - s.y) - (ndc[0].y - s.y) * (ndc[1].x - s.x);
  float3 l = float3(a0 * inv_w[0], a1 * inv_w[1], a2 * inv_w[2]);
  float sum = l.x + l.y + l.z;
  l = abs(sum) > 1e-12 ? l / sum : float3(1.0, 0.0, 0.0);

  float3 normal = normalize(n[0] * l.x + n[1] * l.y + n[2] * l.z);
  float ndl = saturate(dot(normal, normalize(float3(0.4, 1.0, 0.3))));

  float3 base;
  if (p.debug == 1) {
    base = VgeoMeshletColor(entry.x);
  } else if (p.debug == 2) {
    base = VgeoMeshletColor(m.lod * 7u + 3u);
  } else if (p.debug == 3) {
    base = sw ? float3(1.0, 0.45, 0.1) : float3(0.15, 0.5, 1.0);
  } else if (p.hiz.w > 0.0) {
    // Screen-space uv derivatives drive the mip pick; they go wrong only in
    // the quads straddling a silhouette, where neighboring pixels belong to
    // another surface.
    float3 world = wp[0] * l.x + wp[1] * l.y + wp[2] * l.z;
    float2 uv = world.xz * p.hiz.w + 0.5;
    base = albedo.Sample(albedo_sampler, uv).rgb;
  } else {
    base = VgeoMeshletColor(entry.x);
  }

  PsOut o;
  o.color = float4(base * (0.35 + 0.65 * ndl), 1.0);
  o.depth = asfloat(uint(value >> 32));
  return o;
}
