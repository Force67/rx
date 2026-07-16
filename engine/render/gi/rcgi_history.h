#ifndef RX_RENDER_RCGI_HISTORY_H_
#define RX_RENDER_RCGI_HISTORY_H_

#include "core/math.h"

namespace rx::render {

struct RcgiCascadeValidity {
  u32 before_blend;
  u32 after_blend;
};

constexpr RcgiCascadeValidity ComputeRcgiCascadeValidity(u32 previous, u32 current, bool reset) {
  const u32 bit = 1u << current;
  const u32 before = reset ? previous & ~bit : previous;
  return {before, before | bit};
}

struct RcgiHistoryLighting {
  bool authored_interior = false;
  bool interior_miss = false;
  Vec3 interior_ambient{};
  Vec3 sun_direction{};
  f32 sun_intensity = 0;
  Vec3 sun_color{};
};

inline bool ShouldInvalidateRcgiHistory(const RcgiHistoryLighting& previous,
                                        const RcgiHistoryLighting& current) {
  if (previous.authored_interior != current.authored_interior) return true;
  if (!current.authored_interior) return false;
  if (previous.interior_miss != current.interior_miss) return true;
  if (previous.sun_direction.x != current.sun_direction.x ||
      previous.sun_direction.y != current.sun_direction.y ||
      previous.sun_direction.z != current.sun_direction.z ||
      previous.sun_intensity != current.sun_intensity ||
      previous.sun_color.x != current.sun_color.x || previous.sun_color.y != current.sun_color.y ||
      previous.sun_color.z != current.sun_color.z) {
    return true;
  }
  return current.interior_miss &&
         (previous.interior_ambient.x != current.interior_ambient.x ||
          previous.interior_ambient.y != current.interior_ambient.y ||
          previous.interior_ambient.z != current.interior_ambient.z);
}

}  // namespace rx::render

#endif  // RX_RENDER_RCGI_HISTORY_H_
