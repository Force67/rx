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
  f32 wind_speed = 12.53f;   // m/s
  f32 vertical_skew = 700.0f;  // metres of extra downwind drift at the layer top

  f32 turbulence = 1.0f;     // curl-noise distortion of the erosion detail
  f32 density = 1.0f;        // global density multiplier
  // Shell altitudes. The base sits well above the legacy clouds pass (1500 m)
  // so the deck reads overhead rather than crowding the horizon, and the top
  // leaves room for real vertical development on storm towers.
  f32 bottom = 2600.0f;      // layer base, metres above sea level
  f32 top = 8200.0f;         // layer top
  // Storminess 0..1: flattens tops toward an anvil profile and raises the
  // sun absorption so precipitating decks go dark-bottomed.
  f32 anvil = 0.0f;
};

}  // namespace rx::render

#endif  // RX_RENDER_CLOUDSCAPE_TYPES_H_
