float Rand(inout uint state) {
  state = state * 1664525u + 1013904223u;
  return float(state >> 8) / 16777216.0;
}
#include "path_continue.hlsli"

[[vk::binding(0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> result : register(u0, space0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint rng = tid.y * 128u + tid.x;
  float3 throughput = float3(0.001, 0.002, 0.003);
  bool survived = ContinuePath(throughput, rng);
  result[tid.xy] = float4(survived ? throughput : 0.0.xxx, 1.0);
}
