#ifndef RX_PLACEMENT_ECOTOPE_H_
#define RX_PLACEMENT_ECOTOPE_H_


#include <base/containers/vector.h>

#include "base/strings/xstring.h"
#include "foundation/build_config/types.h"
#include "placement/density_program.h"

namespace rx::placement {

// One placement target: a hand-authored asset plus the rules that decide
// where it appears and how individual instances vary. The footprint is the
// effective diameter the object occupies in the placement pattern - a 6 m
// tree and a 1 m fern run through the same algorithm at different scales.
struct PlacementLayer {
  base::String name;
  u64 mesh = 0;  // renderer mesh id instances are drawn with
  f32 footprint = 1.0f;

  DensityProgram density;

  // Per-instance variation, seeded from the stable instance identity.
  f32 scale_min = 1.0f;
  f32 scale_max = 1.0f;
  f32 tilt = 0.0f;  // 0 = upright, 1 = aligned to the surface normal
  f32 y_offset = 0.0f;
  bool random_yaw = true;
};

// A reusable environment description: the layers that make up one kind of
// place (forest, meadow, scree slope). Ecotopes are authored once and
// applied wherever their masks say they belong; layers with the same
// footprint are stacked into one dither interval, which is what keeps a
// pine and a fir from ever claiming the same sample point.
struct Ecotope {
  base::String name;
  base::Vector<PlacementLayer> layers;
};

}  // namespace rx::placement

#endif  // RX_PLACEMENT_ECOTOPE_H_
