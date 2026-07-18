#ifndef RX_RENDER_CLOUDSCAPE_TYPES_H_
#define RX_RENDER_CLOUDSCAPE_TYPES_H_

#include "core/math.h"
#include "core/types.h"

namespace rx::render {

// One endpoint of the world-space weather map blend. The map is generated on
// the GPU from these scalars (a seeded field of coverage / precipitation /
// cloud type), so a weather state is fully described by four numbers and two
// states cross-fade by regenerating the map with a blend factor.
struct CloudscapeMapState {
  u32 seed = 1u;            // varies the spatial pattern per state
  f32 coverage = 0.5f;      // 0 clear .. 1 overcast (mean of the map's R)
  f32 cloud_type = 0.6f;    // 0 stratus .. 0.5 stratocumulus .. 1 cumulus
  f32 precipitation = 0.0f; // 0..1, raises dark storm cells in the map
};

// Everything the weather layer hands the cloudscape renderer each frame.
// The weather system does not know how the clouds are marched or lit; the
// renderer does not know about regions, schedules or transitions. This struct
// is the entire interface between them.
struct CloudscapeControls {
  // Weather map blend: the active map cross-fades from state a to state b as
  // map_blend goes 0 -> 1. Outside a transition a == b and map_blend == 0.
  CloudscapeMapState map_a;
  CloudscapeMapState map_b;
  f32 map_blend = 0.0f;

  // World-space scroll of the weather map (metres), integrated wind advection
  // so formations travel with the wind across state changes.
  Vec2 map_offset{0.0f, 0.0f};

  // Wind advection of the density field itself. Direction the wind blows
  // toward, radians on XZ (matches WeatherSettings::wind_yaw).
  f32 wind_yaw = 0.29146f;
  f32 wind_speed = 12.53f;    // m/s
  f32 vertical_skew = 700.0f; // metres of extra downwind drift at the layer top

  f32 turbulence = 1.0f; // curl-noise distortion of the erosion detail
  f32 density = 1.0f;    // global density multiplier
  // Shell altitudes, metres ASL, authored per weather state to match the real
  // vertical structure of each cloud class: stratus ceilings sit near the
  // surface and stay thin (base up to ~1.2 km, a few hundred metres deep),
  // stratocumulus decks run ~0.6-1.5 km base with tops near 2.5 km, cumulus
  // bases lift with the condensation level (~0.5-2 km, approaching 2.7 km in
  // dry climates) with fair-weather tops a further 1-3 km up, and
  // cumulonimbus towers climb from a low base to a 10-16 km anvil. Defaults
  // describe a dry-climate cumulus deck.
  f32 bottom = 1800.0f; // layer base
  f32 top = 6000.0f;    // layer top
  // Storminess 0..1: flattens tops toward an anvil profile and raises the
  // sun absorption so precipitating decks go dark-bottomed.
  f32 anvil = 0.0f;
  // Menace 0..1: blackens the deck beyond what physics alone gives -- sun
  // absorption climbs and the ambient/multi-scatter floors collapse, so a
  // severe-storm sky reads genuinely threatening instead of merely grey.
  f32 darkness = 0.0f;

  // Ground haze: an exponential height-fog layer that shares the deck's
  // weather. Density modulates with the weather map (humid cells fog up), the
  // baked noise drifts mist banks with the wind, the in-scatter takes the
  // same sunset-tinted ambient the clouds use and dims under thick decks.
  // The weather layer raises it automatically for post-rain mist.
  f32 fog_density = 0.0f; // 0 clear .. 1 thick murk
  f32 fog_height = 90.0f; // exponential falloff scale, metres
  f32 fog_ground = 0.0f;  // the layer's floor altitude, metres ASL
  // Vertical churn 0..1: scrolls the mist banks' noise upward so the layer
  // roils like rising vapour instead of only sliding downwind. Low for a
  // settled morning layer, high over warm standing water.
  f32 fog_churn = 0.15f;

  // Active tornado, written by the weather layer's vortex lifecycle (not
  // authored directly per state -- states only opt in via tornado proneness).
  // strength ramps 0 -> 1 on touchdown and back to 0 as the funnel ropes out;
  // the renderer draws nothing at 0.
  Vec2 tornado_pos{0.0f, 0.0f}; // funnel axis on the ground, world XZ
  f32 tornado_strength = 0.0f;  // 0 none .. 1 fully developed
  f32 tornado_radius = 60.0f;   // wall radius at mid height, metres
};

} // namespace rx::render

#endif // RX_RENDER_CLOUDSCAPE_TYPES_H_
