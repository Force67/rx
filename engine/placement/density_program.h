#ifndef RX_PLACEMENT_DENSITY_PROGRAM_H_
#define RX_PLACEMENT_DENSITY_PROGRAM_H_

#include <span>

#include <base/containers/vector.h>

#include "core/export.h"
#include "core/types.h"

namespace rx::placement {

class WorldData;

// Density programs are the compiled form of the authored placement logic: a
// tiny stack machine evaluated per density texel, on the GPU (DENSITYMAP
// stage) and by the CPU reference evaluator. Keeping the logic as data means
// authored rules ship as buffers, not shader permutations, and the same
// program can be single-stepped for debugging.
enum class DensityOpCode : u32 {
  kConst = 0,     // push a
  kSampleMap,     // push bilinear sample of world map (u32)a at the texel pos
  kNoise,         // push value noise: a = feature size in meters, b = seed
  kMul,           // pop y, pop x, push x*y
  kAdd,           // pop y, pop x, push x+y
  kSub,           // pop y, pop x, push x-y
  kMin,           // pop y, pop x, push min(x,y)
  kMax,           // pop y, pop x, push max(x,y)
  kOneMinus,      // pop x, push 1-x
  kClamp01,       // pop x, push saturate(x)
  kSmoothstep,    // pop x, push smoothstep(a, b, x)
  kRange,         // pop x, push saturate((x-a)/(b-a)); decodes painted bands
  kPow,           // pop x, push pow(max(x,0), a)
};

// One instruction, GPU-mirrored as uint4 {op, asuint(a), asuint(b), asuint(c)}.
struct DensityOp {
  DensityOpCode op = DensityOpCode::kConst;
  f32 a = 0;
  f32 b = 0;
  f32 c = 0;
};

inline constexpr u32 kDensityStackDepth = 8;

// Fluent builder so authoring code reads like the postfix program it emits:
//   DensityProgram().Map(kForest).Map(kRoad).OneMinus().Mul();
class RX_PLACEMENT_EXPORT DensityProgram {
 public:
  DensityProgram& Const(f32 v) { return Push({DensityOpCode::kConst, v}); }
  DensityProgram& Map(u32 map_index) {
    return Push({DensityOpCode::kSampleMap, static_cast<f32>(map_index)});
  }
  DensityProgram& Noise(f32 feature_size_m, f32 seed) {
    return Push({DensityOpCode::kNoise, feature_size_m, seed});
  }
  DensityProgram& Mul() { return Push({DensityOpCode::kMul}); }
  DensityProgram& Add() { return Push({DensityOpCode::kAdd}); }
  DensityProgram& Sub() { return Push({DensityOpCode::kSub}); }
  DensityProgram& Min() { return Push({DensityOpCode::kMin}); }
  DensityProgram& Max() { return Push({DensityOpCode::kMax}); }
  DensityProgram& OneMinus() { return Push({DensityOpCode::kOneMinus}); }
  DensityProgram& Clamp01() { return Push({DensityOpCode::kClamp01}); }
  DensityProgram& Smoothstep(f32 edge0, f32 edge1) {
    return Push({DensityOpCode::kSmoothstep, edge0, edge1});
  }
  DensityProgram& Range(f32 low, f32 high) {
    return Push({DensityOpCode::kRange, low, high});
  }
  DensityProgram& Pow(f32 exponent) { return Push({DensityOpCode::kPow, exponent}); }

  std::span<const DensityOp> ops() const { return {ops_.data(), ops_.size()}; }
  bool empty() const { return ops_.empty(); }

 private:
  DensityProgram& Push(DensityOp op) {
    ops_.push_back(op);
    return *this;
  }

  base::Vector<DensityOp> ops_;
};

// CPU reference evaluator; mirrors placement_density.cs.hlsl op for op. The
// result is already saturated (density is a probability).
RX_PLACEMENT_EXPORT f32 EvalDensityProgram(std::span<const DensityOp> ops,
                                           const WorldData& world, f32 world_x,
                                           f32 world_z);

}  // namespace rx::placement

#endif  // RX_PLACEMENT_DENSITY_PROGRAM_H_
