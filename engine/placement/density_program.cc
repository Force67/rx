#include "placement/density_program.h"

#include <algorithm>
#include <cmath>

#include "placement/placement_math.h"
#include "placement/world_data.h"

namespace rx::placement {

f32 EvalDensityProgram(std::span<const DensityOp> ops, const WorldData& world, f32 world_x,
                       f32 world_z) {
  f32 stack[kDensityStackDepth] = {};
  u32 top = 0;
  auto push = [&](f32 v) {
    if (top < kDensityStackDepth) stack[top++] = v;
  };
  auto pop = [&]() -> f32 { return top > 0 ? stack[--top] : 0.0f; };
  for (const DensityOp& op : ops) {
    switch (op.op) {
      case DensityOpCode::kConst:
        push(op.a);
        break;
      case DensityOpCode::kSampleMap: {
        u32 map = static_cast<u32>(op.a);
        push(map < world.map_count() ? world.Sample(map, world_x, world_z) : 0.0f);
        break;
      }
      case DensityOpCode::kNoise:
        push(ValueNoise(world_x, world_z, op.a, static_cast<u32>(op.b)));
        break;
      case DensityOpCode::kMul: {
        f32 y = pop();
        push(pop() * y);
        break;
      }
      case DensityOpCode::kAdd: {
        f32 y = pop();
        push(pop() + y);
        break;
      }
      case DensityOpCode::kSub: {
        f32 y = pop();
        push(pop() - y);
        break;
      }
      case DensityOpCode::kMin: {
        f32 y = pop();
        push(std::min(pop(), y));
        break;
      }
      case DensityOpCode::kMax: {
        f32 y = pop();
        push(std::max(pop(), y));
        break;
      }
      case DensityOpCode::kOneMinus:
        push(1.0f - pop());
        break;
      case DensityOpCode::kClamp01:
        push(std::clamp(pop(), 0.0f, 1.0f));
        break;
      case DensityOpCode::kSmoothstep: {
        f32 x = pop();
        f32 span = op.b - op.a;
        f32 t = std::clamp(span != 0.0f ? (x - op.a) / span : 0.0f, 0.0f, 1.0f);
        push(t * t * (3.0f - 2.0f * t));
        break;
      }
      case DensityOpCode::kRange: {
        f32 x = pop();
        f32 span = op.b - op.a;
        push(std::clamp(span != 0.0f ? (x - op.a) / span : 0.0f, 0.0f, 1.0f));
        break;
      }
      case DensityOpCode::kPow:
        push(std::pow(std::max(pop(), 0.0f), op.a));
        break;
    }
  }
  return std::clamp(top > 0 ? stack[top - 1] : 0.0f, 0.0f, 1.0f);
}

}  // namespace rx::placement
