#include "rhi_bindings.hlsli"
#include "placement_common.hlsli"
// DENSITYMAP: evaluates the compiled density programs of one dither stack
// over one world tile. Each thread owns one of the 64x64 density texels and
// walks the stack's layers in order, interpreting their bytecode against the
// WorldData maps and accumulating the clamped cumulative density that layered
// dithering keys on: layer k owns the threshold interval [cum[k-1], cum[k])
// at that texel, so same-footprint layers can never claim the same sample.

// DensityOp: {op, asfloat(a), asfloat(b), asfloat(c)}.
[[vk::binding(0, 0)]] StructuredBuffer<uint4> ops : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<PlacementLayerGpu> layers : register(t1, space0);
[[vk::binding(2, 0)]] RWStructuredBuffer<float> density : register(u2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2DArray<float> world_maps : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState world_sampler : register(s3, space0);

struct PushData {
  float2 tile_origin;    // world-space min corner of the tile
  float tile_size;
  float pad0;
  float2 map_origin;     // WorldData region min corner
  float map_inv_extent;  // 1 / region extent
  float pad1;
  uint first_layer;
  uint layer_count;
  uint density_offset;   // float index of this job's density slab
  uint pad2;
};
PUSH_CONSTANTS(PushData, push);

#define OP_CONST 0
#define OP_SAMPLE_MAP 1
#define OP_NOISE 2
#define OP_MUL 3
#define OP_ADD 4
#define OP_SUB 5
#define OP_MIN 6
#define OP_MAX 7
#define OP_ONE_MINUS 8
#define OP_CLAMP01 9
#define OP_SMOOTHSTEP 10
#define OP_RANGE 11
#define OP_POW 12

float SampleWorldMap(uint map, float2 world_pos) {
  float2 uv = (world_pos - push.map_origin) * push.map_inv_extent;
  return world_maps.SampleLevel(world_sampler, float3(uv, float(map)), 0.0);
}

float EvalProgram(uint prog_offset, uint prog_count, float2 world_pos) {
  float stack[PLACEMENT_DENSITY_STACK_DEPTH];
  uint top = 0;
  for (uint i = 0; i < prog_count; ++i) {
    uint4 raw = ops[prog_offset + i];
    uint op = raw.x;
    float a = asfloat(raw.y);
    float b = asfloat(raw.z);
    float x;
    float y;
    switch (op) {
      case OP_CONST:
        if (top < PLACEMENT_DENSITY_STACK_DEPTH) stack[top++] = a;
        break;
      case OP_SAMPLE_MAP:
        if (top < PLACEMENT_DENSITY_STACK_DEPTH) stack[top++] = SampleWorldMap(uint(a), world_pos);
        break;
      case OP_NOISE:
        if (top < PLACEMENT_DENSITY_STACK_DEPTH)
          stack[top++] = PlacementValueNoise(world_pos.x, world_pos.y, a, uint(b));
        break;
      case OP_MUL:
        y = top > 0 ? stack[--top] : 0.0;
        x = top > 0 ? stack[--top] : 0.0;
        stack[top++] = x * y;
        break;
      case OP_ADD:
        y = top > 0 ? stack[--top] : 0.0;
        x = top > 0 ? stack[--top] : 0.0;
        stack[top++] = x + y;
        break;
      case OP_SUB:
        y = top > 0 ? stack[--top] : 0.0;
        x = top > 0 ? stack[--top] : 0.0;
        stack[top++] = x - y;
        break;
      case OP_MIN:
        y = top > 0 ? stack[--top] : 0.0;
        x = top > 0 ? stack[--top] : 0.0;
        stack[top++] = min(x, y);
        break;
      case OP_MAX:
        y = top > 0 ? stack[--top] : 0.0;
        x = top > 0 ? stack[--top] : 0.0;
        stack[top++] = max(x, y);
        break;
      case OP_ONE_MINUS:
        x = top > 0 ? stack[--top] : 0.0;
        stack[top++] = 1.0 - x;
        break;
      case OP_CLAMP01:
        x = top > 0 ? stack[--top] : 0.0;
        stack[top++] = saturate(x);
        break;
      case OP_SMOOTHSTEP: {
        x = top > 0 ? stack[--top] : 0.0;
        float span = b - a;
        float t = saturate(span != 0.0 ? (x - a) / span : 0.0);
        stack[top++] = t * t * (3.0 - 2.0 * t);
        break;
      }
      case OP_RANGE: {
        x = top > 0 ? stack[--top] : 0.0;
        float span = b - a;
        stack[top++] = saturate(span != 0.0 ? (x - a) / span : 0.0);
        break;
      }
      case OP_POW:
        x = top > 0 ? stack[--top] : 0.0;
        stack[top++] = pow(max(x, 0.0), a);
        break;
      default:
        break;
    }
  }
  return saturate(top > 0 ? stack[top - 1] : 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= PLACEMENT_DENSITY_RES || id.y >= PLACEMENT_DENSITY_RES) return;
  float texel_size = push.tile_size / float(PLACEMENT_DENSITY_RES);
  float2 world_pos = push.tile_origin + (float2(id.xy) + 0.5) * texel_size;

  float cumulative = 0.0;
  for (uint layer = 0; layer < push.layer_count; ++layer) {
    PlacementLayerGpu desc = layers[push.first_layer + layer];
    cumulative += EvalProgram(desc.prog_offset, desc.prog_count, world_pos);
    cumulative = min(cumulative, 1.0);
    uint slab = push.density_offset +
                (layer * PLACEMENT_DENSITY_RES + id.y) * PLACEMENT_DENSITY_RES + id.x;
    density[slab] = cumulative;
  }
}
