#include "rhi_bindings.hlsli"
#include "vgeo_common.hlsli"
// Folds the cull counters into indirect dispatch/mesh-task records (a single
// thread; the counts are only known once the preceding cull finished).
// mode 0 runs after the main cull: sw/hw raster args + the post-cull dispatch,
// and snapshots the list sizes so the post raster can draw just its deltas.
// mode 1 runs after the post cull: raster args for the entries appended since.

[[vk::binding(0, 0)]] StructuredBuffer<VgeoParams> params_buf : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> counters : register(u1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> args : register(u2, space0);

struct PushData {
  uint mode;
};
PUSH_CONSTANTS(PushData, push);

[numthreads(1, 1, 1)]
void main() {
  VgeoParams p = params_buf[0];
  uint sw = min(counters[VGEO_COUNTER_SW], p.max_visible);
  uint hw = min(counters[VGEO_COUNTER_HW], p.max_visible);
  if (push.mode == 0) {
    uint occ = min(counters[VGEO_COUNTER_OCCLUDED], p.max_visible);
    counters[VGEO_COUNTER_OCCLUDED] = occ;  // clamp for the post-cull bounds check
    args[VGEO_ARGS_SW_MAIN + 0] = sw;
    args[VGEO_ARGS_SW_MAIN + 1] = 1;
    args[VGEO_ARGS_SW_MAIN + 2] = 1;
    args[VGEO_ARGS_HW_MAIN + 0] = hw;
    args[VGEO_ARGS_HW_MAIN + 1] = 1;
    args[VGEO_ARGS_HW_MAIN + 2] = 1;
    args[VGEO_ARGS_POST_CULL + 0] = (occ + 63) / 64;
    args[VGEO_ARGS_POST_CULL + 1] = 1;
    args[VGEO_ARGS_POST_CULL + 2] = 1;
    counters[VGEO_COUNTER_SW_BASE] = sw;
    counters[VGEO_COUNTER_HW_BASE] = hw;
  } else {
    args[VGEO_ARGS_SW_POST + 0] = sw - counters[VGEO_COUNTER_SW_BASE];
    args[VGEO_ARGS_SW_POST + 1] = 1;
    args[VGEO_ARGS_SW_POST + 2] = 1;
    args[VGEO_ARGS_HW_POST + 0] = hw - counters[VGEO_COUNTER_HW_BASE];
    args[VGEO_ARGS_HW_POST + 1] = 1;
    args[VGEO_ARGS_HW_POST + 2] = 1;
  }
}
