#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
// RCGI probe relocation (Phase 3 item 10). One thread per probe of the current
// (frame % cascades) cascade. Reads back this frame's visibility rays and moves
// the probe out of nearby geometry, persisting a small world-space offset in the
// per-probe metadata the trace / blend / gather all read. Runs after the border
// pass, so it observes rays traced from the *current* (already relocated)
// position and nudges incrementally toward the open hemisphere for stability.
//
// Rays buffer convention (rcgi_probe_trace): .a > 0 front hit at that distance,
// .a < 0 miss (open, |a| = max), .a == 0 backface (started inside geometry).

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> rays : register(u0, space0);
[[vk::binding(1, 0)]] ConstantBuffer<RcgiGlobals> rcgi : register(b1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint2> probe_meta : register(u2, space0);

struct PushData {
  float4 rotation_x;
  float4 rotation_y;
  float4 rotation_z;
  uint reset;  // 1 = cascade just (re)snapped: start the offset from the cell center
  uint3 pad;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint probe_count = kRcgiProbesPerAxis * kRcgiProbesPerAxis * kRcgiProbesPerAxis;
  if (id.x >= probe_count) return;

  uint cascade = rcgi.misc.x;
  uint ray_count = (uint)rcgi.sun_color.w;
  uint3 probe = RcgiProbeFromIndex(id.x);
  uint meta_idx = RcgiMetaIndex(cascade, probe);

  uint backface = 0u;
  float3 escape = 0.0.xxx;  // sum of ray dirs weighted by openness (- when blocked)
  for (uint r = 0u; r < ray_count; ++r) {
    float3 fib = RcgiFibonacci(r, ray_count);
    float3 dir = normalize(float3(dot(push.rotation_x.xyz, fib), dot(push.rotation_y.xyz, fib),
                                  dot(push.rotation_z.xyz, fib)));
    float w = rays[int2(r, id.x)].a;
    if (w == 0.0) {
      backface += 1u;
      escape -= dir;  // geometry immediately in this direction: push away from it
    } else if (w < 0.0) {
      escape += dir;  // clean miss: fully open toward dir
    } else {
      escape += dir * saturate(w / max(rcgi.params.x, 1e-3));  // partial openness
    }
  }
  float backface_frac = float(backface) / float(max(ray_count, 1u));

  // Relocate only when a meaningful fraction of rays start inside geometry
  // (AC Shadows use ~25%); otherwise relax back toward the cell center.
  float3 target = 0.0.xxx;
  if (backface_frac > 0.25 && dot(escape, escape) > 1e-6) {
    target = normalize(escape) * kRcgiRelocMaxOffset;
  }

  float3 cur = push.reset != 0u ? 0.0.xxx : RcgiUnpackOffset(probe_meta[meta_idx].x);
  float3 next = lerp(cur, target, 0.25);  // incremental for temporal stability

  // Unrecoverable: still mostly backfaces even after relocation -> disable so the
  // irradiance sample gives this probe zero weight.
  uint flags = backface_frac > 0.6 ? kRcgiMetaDisabled : 0u;
  probe_meta[meta_idx] = uint2(RcgiPackOffset(next), flags);
}
