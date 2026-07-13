#include "rhi_bindings.hlsli"
// Builds the indirect dispatch args for the cache-shade pass from the active
// cell count the probe trace accumulated. Single thread: clamps the count to
// the active-list capacity and writes (ceil(count/64), 1, 1).

#define RX_RCGI_ACTIVE_CAP (1u << 18)

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> active_meta : register(u0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> dispatch_args : register(u1, space0);

[numthreads(1, 1, 1)]
void main() {
  uint count = min(active_meta[0], RX_RCGI_ACTIVE_CAP);
  active_meta[0] = count;  // clamped bound for the shade pass
  dispatch_args[0] = (count + 63u) / 64u;
  dispatch_args[1] = 1u;
  dispatch_args[2] = 1u;
}
