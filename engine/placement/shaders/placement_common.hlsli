// Shared definitions of the procedural placement pipeline. Every constant and
// hash here mirrors engine/placement/placement_math.h and placement.h; the
// integer math is bit-exact with the CPU reference generator so a tile
// produces the same instances no matter which path emitted it.
#ifndef RX_PLACEMENT_COMMON_HLSLI_
#define RX_PLACEMENT_COMMON_HLSLI_

#define PLACEMENT_DENSITY_RES 64
#define PLACEMENT_PATTERN_POINTS 256
#define PLACEMENT_MAX_STACK_LAYERS 8
#define PLACEMENT_DENSITY_STACK_DEPTH 8

// Mirrors placement::PlacementLayer's GPU view (LayerGpu in gpu_placement.cc).
struct PlacementLayerGpu {
  uint prog_offset;
  uint prog_count;
  uint flags;  // 1 = random yaw
  uint pad;
  float scale_min;
  float scale_max;
  float tilt;
  float y_offset;
};

// One oriented point between GENERATE and PLACEMENT (3 float4 = 48 B).
struct PlacementPoint {
  float3 position;
  float pad0;
  float3 normal;
  uint layer;  // global layer index
  uint rank;   // pattern rank = threshold order
  int tile_x;
  int tile_z;
  uint pad1;
};

// One finished instance in the host-visible result buffer (80 B).
struct PlacementInstance {
  column_major float4x4 transform;
  uint layer;
  uint rank;
  int tile_x;
  int tile_z;
};

uint PlacementPcg(uint v) {
  uint state = v * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}

uint PlacementHashCombine(uint a, uint b) { return PlacementPcg(a ^ (b * 0x9E3779B9u)); }

float PlacementHashToUnit(uint h) { return float(h >> 8u) * (1.0 / 16777216.0); }

uint PlacementInstanceSeed(uint world_seed, int tile_x, int tile_z, uint pt, uint layer) {
  uint h = PlacementHashCombine(world_seed, uint(tile_x) * 0x8DA6B343u);
  h = PlacementHashCombine(h, uint(tile_z) * 0xD8163841u);
  h = PlacementHashCombine(h, pt * 0xCB1AB31Fu);
  return PlacementHashCombine(h, layer + 1u);
}

float PlacementSmoothstep01(float x) {
  x = saturate(x);
  return x * x * (3.0 - 2.0 * x);
}

// Value noise on an integer lattice; mirrors placement::ValueNoise.
float PlacementValueNoise(float x, float z, float feature_size, uint seed) {
  if (feature_size <= 0.0) return 0.5;
  float fx = x / feature_size;
  float fz = z / feature_size;
  float ix = floor(fx);
  float iz = floor(fz);
  float tx = PlacementSmoothstep01(fx - ix);
  float tz = PlacementSmoothstep01(fz - iz);
  int cx = int(ix);
  int cz = int(iz);
  float v00 = PlacementHashToUnit(
      PlacementHashCombine(PlacementHashCombine(seed, uint(cx) * 0x8DA6B343u),
                           uint(cz) * 0xD8163841u));
  float v10 = PlacementHashToUnit(
      PlacementHashCombine(PlacementHashCombine(seed, uint(cx + 1) * 0x8DA6B343u),
                           uint(cz) * 0xD8163841u));
  float v01 = PlacementHashToUnit(
      PlacementHashCombine(PlacementHashCombine(seed, uint(cx) * 0x8DA6B343u),
                           uint(cz + 1) * 0xD8163841u));
  float v11 = PlacementHashToUnit(
      PlacementHashCombine(PlacementHashCombine(seed, uint(cx + 1) * 0x8DA6B343u),
                           uint(cz + 1) * 0xD8163841u));
  float a = v00 + (v10 - v00) * tx;
  float b = v01 + (v11 - v01) * tx;
  return a + (b - a) * tz;
}

#endif  // RX_PLACEMENT_COMMON_HLSLI_
