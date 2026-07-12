#include "rhi_bindings.hlsli"
#include "vgeo_common.hlsli"
// Clears the visibility buffer to empty (0) and, from the first thread, the
// cull counters. One thread per pixel, 64-wide groups over a flat index.

[[vk::binding(0, 0)]] RWStructuredBuffer<uint64_t> visbuffer : register(u0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> counters : register(u1, space0);

struct PushData {
  uint pixel_count;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x == 0) {
    [unroll]
    for (uint c = 0; c < 8; ++c) counters[c] = 0;
  }
  if (id.x < push.pixel_count) visbuffer[id.x] = 0;
}
