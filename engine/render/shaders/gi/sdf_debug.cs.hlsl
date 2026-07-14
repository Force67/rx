#include "rhi_bindings.hlsli"
#include "gi/sdf_trace.hlsli"
// Debug visualisation of the global SDF clipmap, composited straight onto the
// lit scene colour (a late render-graph pass). Casts one camera ray per pixel
// and sphere-traces the clipmap. RX_SDF_DEBUG=1 shows the distance field (tint
// by clip + gradient shading + distance bands); RX_SDF_DEBUG=2 shows the hit
// albedo lit by the gradient normal -- a crude "SDF view of the scene", the S1
// acceptance test. This is the primary standalone verification tool for S1.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> lit : register(u0, space0);
[[vk::binding(1, 0)]] ConstantBuffer<SdfGlobals> sdf : register(b1, space0);
[[vk::binding(2, 0)]] Texture3D<float> sdf_distance : register(t2, space0);
[[vk::binding(3, 0)]] Texture3D<float4> sdf_albedo : register(t3, space0);
[[vk::binding(4, 0)]] Texture3D<float4> sdf_emissive : register(t4, space0);
[[vk::binding(5, 0)]] SamplerState sdf_sampler : register(s5, space0);

struct DebugPush {
  column_major float4x4 inv_view_proj;  // unjittered
  float4 camera_pos;  // xyz eye
  float4 params;      // x inv_w, y inv_h, z tmax
  uint4 misc;         // x mode (1 distance, 2 albedo)
};
PUSH_CONSTANTS(DebugPush, pc);

static const float3 kClipTint[4] = {
    float3(0.95, 0.35, 0.30), float3(0.35, 0.85, 0.40),
    float3(0.35, 0.55, 0.95), float3(0.90, 0.80, 0.35)};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint w, h;
  lit.GetDimensions(w, h);
  if (id.x >= w || id.y >= h) return;

  float2 uv = (float2(id.xy) + 0.5) * pc.params.xy;
  float2 ndc = uv * 2.0 - 1.0;
  // Reconstruct the ray from the near-plane world point and the camera. Using
  // the far plane (reversed-z depth 0, infinite far) divides by ~0 and yields a
  // NaN direction, so anchor on the near plane (depth 1) instead.
  float4 near_h = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));
  float3 p_near = near_h.xyz / near_h.w;
  float3 origin = pc.camera_pos.xyz;
  float3 dir = normalize(p_near - origin);

  // Diagnostic modes: 3 = raw distance field sampled along the ray at a fixed
  // world distance (green = positive, red = negative, brightness = |d|/2 m);
  // 4 = clip selection at that point (tinted) or black outside all clips.
  SdfHit hit = TraceGlobalSdf(origin, dir, pc.params.z, sdf, sdf_distance, sdf_albedo, sdf_emissive,
                              sdf_sampler);

  float3 color;
  if (hit.miss) {
    color = float3(0.02, 0.02, 0.025);  // pure SDF view: dark where the ray misses
  } else if (pc.misc.x == 2u) {
    float3 L = normalize(float3(0.4, 0.85, 0.35));
    float ndl = saturate(dot(hit.normal, L)) * 0.75 + 0.25;
    color = hit.albedo * ndl + hit.emissive;
  } else {
    uint c = SdfSelectClip(sdf, hit.pos);
    c = min(c, kSdfClips - 1u);
    float3 L = normalize(float3(0.4, 0.85, 0.35));
    float ndl = saturate(dot(hit.normal, L)) * 0.6 + 0.4;
    float band = 0.7 + 0.3 * saturate(cos(hit.hitT * 6.2831853));  // ~1 m distance bands
    color = kClipTint[c] * ndl * band;
  }
  lit[id.xy] = float4(color, 1.0);
}
