#include "rhi_bindings.hlsli"

struct FocusPush {
  float near_plane;
  float focus_speed;
  float focus_override;
  uint reset;
};
PUSH_CONSTANTS(FocusPush, pc);

[[vk::image_format("r32f")]] [[vk::binding(0, 0)]] RWTexture2D<float> focus_state : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);

// One writer, followed by a graph dependency before the per-pixel CoC pass.
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  float target = pc.focus_override;
  if (target <= 0.0) {
    uint width, height;
    depth_map.GetDimensions(width, height);
    float depth = depth_map.Load(int3(width / 2, height / 2, 0));
    target = depth > 0.0 ? pc.near_plane / depth : 100.0;
  }
  float focus = target;
  if (pc.reset == 0u && pc.focus_override <= 0.0) {
    focus = lerp(focus_state[uint2(0, 0)], target, saturate(pc.focus_speed));
  }
  focus_state[uint2(0, 0)] = max(focus, pc.near_plane);
}
