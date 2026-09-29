#ifndef RX_RENDER_RENDERER_INTERNAL_H_
#define RX_RENDER_RENDERER_INTERNAL_H_

#include <base/option.h>

#include "foundation/math/math.h"

// What the renderer_*.cc files share beyond renderer.h. Everything else each
// file needs is private to it.
namespace rx::render::internal {

// RX_RT_VEG: read when a masked mesh uploads and when the frame builds its
// reflection passes. Defined in renderer_frame.cc.
extern base::Option<bool> RtVegOpt;

// Mirrors ContactCamera in contact_shadow.cs: the march's two unjittered
// camera matrices, which on their own would exhaust the push budget.
struct ContactCamera {
  Mat4 view_proj;
  Mat4 inv_view_proj;
};

}  // namespace rx::render::internal

#endif  // RX_RENDER_RENDERER_INTERNAL_H_
