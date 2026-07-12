#include "anim/morph.h"

#include <algorithm>

namespace rx::anim {

void SampleMorphWeights(const asset::MorphAnimation& animation, f32 time,
                        base::Vector<f32>* out) {
  const u32 keys = static_cast<u32>(animation.times.size());
  const u32 targets = keys > 0 ? static_cast<u32>(animation.weights.size()) / keys : 0;
  out->resize(targets);
  if (targets == 0) return;
  std::fill(out->begin(), out->end(), 0.0f);
  if (animation.weights.size() < static_cast<size_t>(keys) * targets) return;

  // Last key at or before `time`; clamped so times outside the track hold the
  // first/last pose.
  u32 key = 0;
  while (key + 1 < keys && animation.times[key + 1] <= time) ++key;
  const f32* row = &animation.weights[static_cast<size_t>(key) * targets];
  if (animation.step || key + 1 >= keys || time <= animation.times[key]) {
    std::copy(row, row + targets, out->data());
    return;
  }
  f32 span = animation.times[key + 1] - animation.times[key];
  f32 t = span > 1e-8f ? std::min((time - animation.times[key]) / span, 1.0f) : 0.0f;
  const f32* next = row + targets;
  for (u32 i = 0; i < targets; ++i) (*out)[i] = row[i] + (next[i] - row[i]) * t;
}

}  // namespace rx::anim
