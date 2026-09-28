#include "rhi_bindings.hlsli"
// Motion blur, stage 1: per-tile dominant velocity. Each 16x16 pixel tile
// stores its max-magnitude velocity; the gather pass scans a 3x3 tile
// neighborhood so fast movers bleed blur into neighboring tiles (McGuire).
struct TileMaxPush {
  uint2 tile_count;
  uint2 size;      // output resolution, tiles cover 16x16 output pixels
  float2 scale;    // uv velocity -> blur vector (shutter * direction)
  float2 max_blur; // uv clamp so extreme velocities stay bounded
  float2 debug_vel;  // nonzero: override every velocity (static-camera testing)
  float2 pad;
};
PUSH_CONSTANTS(TileMaxPush, pc);

[[vk::image_format("rg16f")]] [[vk::binding(0, 0)]] RWTexture2D<float2> tile_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float2> motion : register(t1, space0);  // uv curr -> prev

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.tile_count.x || id.y >= pc.tile_count.y) return;
  float2 best = 0.0.xx;
  float best_mag = 0.0;
  uint2 base = id.xy * 16u;
  uint width, height;
  motion.GetDimensions(width, height);
  for (uint y = 0; y < 16u; ++y) {
    for (uint x = 0; x < 16u; ++x) {
      uint2 p = min(base + uint2(x, y), pc.size - 1u);
      float2 uv = (float2(p) + 0.5) / float2(pc.size);
      uint2 source = min(uint2(uv * float2(width, height)), uint2(width, height) - 1u);
      // motion points current -> previous; the blur streaks along the
      // apparent screen motion, i.e. the opposite.
      float2 v = any(pc.debug_vel != 0.0) ? pc.debug_vel
                                          : -motion.Load(int3(source, 0)) * pc.scale;
      v = clamp(v, -pc.max_blur, pc.max_blur);
      float2 pixels = v * float2(pc.size);
      float m = dot(pixels, pixels);
      if (m > best_mag) {
        best_mag = m;
        best = v;
      }
    }
  }
  tile_out[id.xy] = best;
}
