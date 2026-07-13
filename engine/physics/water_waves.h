#ifndef RX_PHYSICS_WATER_WAVES_H_
#define RX_PHYSICS_WATER_WAVES_H_

#include <cmath>

#include "core/math.h"
#include "core/types.h"

// CPU port of the analytic Gerstner wave field. This is the wave PROXY the
// physics side uses to make floating bodies ride the swell: the water-height
// callback fed to PhysicsWorld::set_water_height evaluates it so buoyant bodies
// bob on real waves instead of a flat plane, and their horizontal drift follows
// the wave's orbital flow.
//
// KEEP THESE CONSTANTS IN SYNC with the GPU Gerstner field in
// engine/render/shaders/geometry/water_waves.hlsli (which mesh.vs displaces the
// surface with, water.ps re-evaluates for shading, and shore_wetting.cs.hlsl /
// water_field.cs.hlsl mirror as their wave proxy). If the numbers here drift
// from the shader the floaters will ride a surface that no longer matches the
// rendered water. This mirrors the precedent the shore-wetting compute set: CPU
// systems evaluate the analytic Gerstner even when the FFT ocean is the actual
// displaced surface, as a plausible-phase proxy.

namespace rx::physics {

struct GerstnerWaveParam {
  f32 dir_x, dir_z;   // normalized XZ propagation direction
  f32 wavelength;     // meters
  f32 amplitude;      // meters
};

// Mirror of kGerstner[4] in water_waves.hlsli.
inline constexpr GerstnerWaveParam kGerstnerWaves[4] = {
    {0.780869f, 0.624695f, 19.0f, 0.16f},
    {-0.599997f, 0.800002f, 11.0f, 0.09f},
    {0.953583f, -0.301131f, 6.3f, 0.045f},
    {-0.286206f, -0.958164f, 3.7f, 0.022f},
};
inline constexpr f32 kGerstnerChop = 0.68f;  // mirror of kGerstnerChop
inline constexpr f32 kWaterTau = 6.2831853f;
inline constexpr f32 kGravity = 9.81f;  // deep-water dispersion

// Surface height (world Y offset from the rest plane) of the water column at
// world (x, z) and time t. This samples at the rest position rather than
// solving the Gerstner inverse for the displaced point: the horizontal
// displacement is small (sub-metre) so the rest-position height is an accurate,
// cheap proxy, and it matches how mesh.vs evaluates the field per grid vertex.
//
// Optional outputs:
//  * `flow`   — horizontal orbital velocity (m/s) in xz (y left untouched), so a
//               buoyant body drifts along with the passing swell.
//  * `surface_vy` — vertical velocity of the surface (m/s), so a body slamming
//               into a rising/falling surface can be detected relative to it.
inline f32 GerstnerWaveHeight(f32 x, f32 z, f32 t, Vec3* flow = nullptr,
                              f32* surface_vy = nullptr) {
  f32 height = 0.0f;
  f32 flow_x = 0.0f, flow_z = 0.0f, vy = 0.0f;
  for (const GerstnerWaveParam& g : kGerstnerWaves) {
    const f32 k = kWaterTau / g.wavelength;
    const f32 w = std::sqrt(kGravity * k);  // deep-water dispersion
    const f32 phase = k * (g.dir_x * x + g.dir_z * z) + w * t;
    const f32 s = std::sin(phase);
    const f32 c = std::cos(phase);
    height += g.amplitude * s;
    // Horizontal displacement amplitude q*amp = chop / (4k); its time
    // derivative (orbital velocity) is -dir * q*amp * w * sin(phase).
    const f32 horiz_amp = kGerstnerChop / (4.0f * k);
    flow_x += -g.dir_x * horiz_amp * w * s;
    flow_z += -g.dir_z * horiz_amp * w * s;
    vy += g.amplitude * w * c;  // d/dt of amp*sin(phase)
  }
  if (flow) {
    flow->x = flow_x;
    flow->z = flow_z;
  }
  if (surface_vy) *surface_vy = vy;
  return height;
}

}  // namespace rx::physics

#endif  // RX_PHYSICS_WATER_WAVES_H_
