#ifndef RX_ANIM_MORPH_H_
#define RX_ANIM_MORPH_H_

#include <base/containers/vector.h>

#include "asset/mesh.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::anim {

// Samples a keyframed morph weight animation at `time` (seconds, clamped to
// the key range; loop by wrapping time before the call) into one weight per
// target. Linear between keys, held at the previous key for step tracks.
// `out` is sized to the track's target count.
RX_ANIM_EXPORT void SampleMorphWeights(const asset::MorphAnimation& animation, f32 time,
                                       base::Vector<f32>* out);

// Appends the nonzero entries of a dense per-target weight set as
// (target, weight) pairs, the layout FrameView::morph_weights expects, and
// returns the count appended. `pairs` is any Vector of {u32, f32} structs
// (render::MorphWeight); zero weights are skipped so idle targets cost
// nothing on the GPU.
template <typename Pair>
u32 AppendActiveMorphWeights(const base::Vector<f32>& weights, base::Vector<Pair>* pairs) {
  u32 appended = 0;
  for (u32 i = 0; i < weights.size(); ++i) {
    if (weights[i] == 0.0f) continue;
    pairs->push_back({i, weights[i]});
    ++appended;
  }
  return appended;
}

}  // namespace rx::anim

#endif  // RX_ANIM_MORPH_H_
