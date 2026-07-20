#ifndef RX_RENDER_LIGHTNING_ENVELOPE_H_
#define RX_RENDER_LIGHTNING_ENVELOPE_H_

#include <algorithm>
#include <cmath>

#include "core/types.h"

namespace rx::render {

inline constexpr f32 kLightningStrikeDuration = 0.45f;

// CPU mirror of the deterministic stroke envelope in lightning_bolt shaders.
// Kept header-only so gameplay/weather code can schedule matching flashes
// without linking the GPU renderer implementation.
inline f32 LightningEnvelope(f32 age, u32 seed) {
  if (!std::isfinite(age) || age < 0.0f || age >= kLightningStrikeDuration)
    return 0.0f;
  auto pcg_hash = [](u32 v) {
    v = v * 747796405u + 2891336453u;
    v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (v >> 22u) ^ v;
  };
  u32 a = pcg_hash(seed * 747796405u + 3u);
  u32 b = pcg_hash(a);
  u32 c = pcg_hash(b);
  u32 d = pcg_hash(c);
  constexpr f32 kInv = 1.0f / 4294967295.0f;
  f32 hx = static_cast<f32>(a) * kInv;
  f32 hy = static_cast<f32>(b) * kInv;
  f32 hz = static_cast<f32>(c) * kInv;
  f32 hw = static_cast<f32>(d) * kInv;

  f32 envelope = std::exp(-age * 26.0f);
  f32 t1 = 0.10f + 0.08f * hx;
  if (age > t1)
    envelope += (0.5f + 0.4f * hy) * std::exp(-(age - t1) * 30.0f);
  if (hz > 0.35f) {
    f32 t2 = 0.22f + 0.10f * hw;
    if (age > t2)
      envelope += (0.35f + 0.30f * hx) * std::exp(-(age - t2) * 30.0f);
  }
  return envelope *
         std::clamp((kLightningStrikeDuration - age) * 20.0f, 0.0f, 1.0f);
}

} // namespace rx::render

#endif // RX_RENDER_LIGHTNING_ENVELOPE_H_
