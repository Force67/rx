#include "rhi_bindings.hlsli"
// Radix-2 inverse FFT of one line (row or column) per 256-thread workgroup,
// entirely in shared memory: bit-reversed load, 8 butterfly stages with
// +i twiddles (inverse transform), no normalization (folded into finalize).
// Each texel carries two independent complex signals in xy / zw.
struct FftPush {
  uint size;      // 256
  uint log_size;  // 8
  uint horizontal;
  uint pad0;
};
PUSH_CONSTANTS(FftPush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> data : register(u0, space0);

static const float kTau = 6.2831853;

groupshared float4 ping[256];
groupshared float4 pong[256];

float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint tid : SV_GroupIndex) {
  uint2 coord = pc.horizontal != 0 ? uint2(tid, gid.x) : uint2(gid.x, tid);
  ping[tid] = data[coord];
  GroupMemoryBarrierWithGroupSync();
  // Bit-reversed reorder (8-bit reverse for N=256).
  pong[tid] = ping[reversebits(tid) >> (32u - pc.log_size)];
  GroupMemoryBarrierWithGroupSync();

  bool src_pong = true;
  for (uint s = 1; s <= pc.log_size; ++s) {
    uint m = 1u << s;
    uint mh = m >> 1;
    uint j = tid & (mh - 1);
    uint base = tid & ~(m - 1);
    uint lo = base + j;
    uint hi = lo + mh;
    float ang = kTau * float(j) / float(m);  // +i: inverse transform
    float2 tw = float2(cos(ang), sin(ang));
    float4 a = src_pong ? pong[lo] : ping[lo];
    float4 b = src_pong ? pong[hi] : ping[hi];
    float4 tb = float4(CMul(b.xy, tw), CMul(b.zw, tw));
    float4 result = (tid & mh) != 0 ? a - tb : a + tb;
    GroupMemoryBarrierWithGroupSync();
    if (src_pong) ping[tid] = result; else pong[tid] = result;
    src_pong = !src_pong;
    GroupMemoryBarrierWithGroupSync();
  }
  data[coord] = src_pong ? pong[tid] : ping[tid];
}
