#ifndef RX_INVENTORY_EXPORT_H_
#define RX_INVENTORY_EXPORT_H_

// The inventory module ships as two targets (like rx::nav / rx::nav_viz): the
// physics-free core (rx_inventory) and the world-drop half that pulls physics
// (rx_inventory_world). Each gets its own cross-DSO export macro. Both resolve
// through core/export.h's RX_DSO_EXPORT/RX_DSO_IMPORT, so this header defines
// them locally instead of editing the shared engine/core/export.h (which a
// second agent is touching in parallel).

#include "core/export.h"

#if defined(RX_INVENTORY_IMPLEMENTATION)
#  define RX_INVENTORY_EXPORT RX_DSO_EXPORT
#else
#  define RX_INVENTORY_EXPORT RX_DSO_IMPORT
#endif

#if defined(RX_INVENTORY_WORLD_IMPLEMENTATION)
#  define RX_INVENTORY_WORLD_EXPORT RX_DSO_EXPORT
#else
#  define RX_INVENTORY_WORLD_EXPORT RX_DSO_IMPORT
#endif

#endif  // RX_INVENTORY_EXPORT_H_
