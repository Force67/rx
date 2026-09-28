#define main production_main
#include "gi/recon_gbuffer.cs.hlsl"
#undef main

[[vk::binding(31, 0)]] RWStructuredBuffer<float4> results : register(u31, space0);

[numthreads(1, 1, 1)]
void main() {
  Hit h = TraceClosest(float3(0, 0, -1), float3(0, 0, 1), 0.0, true);
  results[0] = float4(h.previous_position, h.history_valid ? 1.0 : 0.0);
  float4x4 identity = float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
  results[1] = float4(PathMotion(h.previous_position, identity, float2(0, 0), h.history_valid),
                     float(h.inst), h.hit ? 1.0 : 0.0);
}
