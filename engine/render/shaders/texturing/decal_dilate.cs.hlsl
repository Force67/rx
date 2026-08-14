// One-ring gutter fill for a decal tile.
//
// A UV chart's border texels are only partially covered by the rasterizer, so
// the forward pass's bilinear filter reaches texels the bake never wrote and a
// splat crossing a seam shows a hairline crack. This copies the best in-chart
// neighbour into every out-of-chart texel that touches the chart.
//
// It only WRITES texels the chart mask calls outside and only READS ones it
// calls inside, so it is idempotent and free of read/write races: running it
// after every bake cannot make the gutter creep outward.

#include "rhi_bindings.hlsli"

[[vk::image_format("rgba8")]] [[vk::binding(0, 0)]] RWTexture2D<float4> albedo : register(u0, space0);
[[vk::image_format("rgba8")]] [[vk::binding(1, 0)]] RWTexture2D<float4> fx : register(u1, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D chart : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState chart_sampler : register(s2, space0);

struct DilatePush {
  uint2 origin;  // tile's top-left texel in the atlas
  uint size;     // tile edge in texels
  uint pad;
};
PUSH_CONSTANTS(DilatePush, push);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= push.size || tid.y >= push.size) return;
  int2 local = int2(tid.xy);
  int2 coord = int2(push.origin) + local;
  if (chart.Load(int3(coord, 0)).r >= 0.5) return;  // inside a chart: authored

  // Widest-coverage neighbour wins, so a gutter texel picks up the splat rather
  // than a bare patch of the same chart.
  float best = 0.0;
  int2 best_coord = int2(0, 0);
  bool found = false;
  [unroll]
  for (int dy = -1; dy <= 1; ++dy) {
    [unroll]
    for (int dx = -1; dx <= 1; ++dx) {
      int2 n = local + int2(dx, dy);
      // Clamped to this tile: neighbouring tiles belong to other receivers.
      if (n.x < 0 || n.y < 0 || n.x >= int(push.size) || n.y >= int(push.size)) continue;
      int2 nc = int2(push.origin) + n;
      if (chart.Load(int3(nc, 0)).r < 0.5) continue;
      float cov = albedo[nc].a;
      if (!found || cov > best) {
        best = cov;
        best_coord = nc;
        found = true;
      }
    }
  }
  if (!found) return;
  albedo[coord] = albedo[best_coord];
  fx[coord] = fx[best_coord];
}
