#define main production_main
#include "gi/pathtrace_gbuffer.cs.hlsl"
#undef main

[[vk::binding(31, 0)]] RWStructuredBuffer<uint> results : register(u31, space0);

[numthreads(8, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  results[tid.x] = PassesAlpha(0, tid.x, 0, float2(0, 0), 0.0);
}
