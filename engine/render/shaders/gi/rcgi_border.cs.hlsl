#include "rhi_bindings.hlsli"
// RCGI octahedral border duplication for one cascade slab, so bilinear taps at
// probe edges wrap correctly. One thread per slab texel; the slab is offset into
// the full atlas by cascade * slab_height. Mirrors ddgi_border for a 2D atlas.

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> atlas : register(u0, space0);

struct PushData {
  uint texels;    // interior resolution
  uint probes_x;  // probesX * probesZ
  uint probes_y;  // probesY
  uint y_base;    // cascade * slab_height (rows into the atlas)
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint stride = push.texels + 2u;
  if (id.x >= push.probes_x * stride || id.y >= push.probes_y * stride) return;
  uint2 in_probe = uint2(id.x % stride, id.y % stride);
  bool border_x = in_probe.x == 0u || in_probe.x == stride - 1u;
  bool border_y = in_probe.y == 0u || in_probe.y == stride - 1u;
  if (!border_x && !border_y) return;

  uint2 pbase = uint2(id.x - in_probe.x, id.y - in_probe.y);
  uint n = push.texels;
  uint2 src;
  if (border_x && border_y) {
    src = uint2(in_probe.x == 0u ? n : 1u, in_probe.y == 0u ? n : 1u);
  } else if (border_x) {
    src = uint2(in_probe.x == 0u ? 1u : n, n + 1u - in_probe.y);
  } else {
    src = uint2(n + 1u - in_probe.x, in_probe.y == 0u ? 1u : n);
  }
  atlas[uint2(id.x, push.y_base + id.y)] = atlas[uint2(pbase + src) + uint2(0u, push.y_base)];
}
