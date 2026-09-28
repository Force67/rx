#ifndef RX_WEATHER_EXPORT_H_
#define RX_WEATHER_EXPORT_H_

// Per-module export annotation. The weather module post-dates engine/core/
// export.h's fixed macro table, so RX_WEATHER_EXPORT is derived here from the
// shared RX_DSO_* primitives using the same RX_<MODULE>_IMPLEMENTATION selector
// rx_add_module() defines. In the default static build this expands to nothing.

#include "core/export.h"

#if defined(RX_WEATHER_IMPLEMENTATION)
#define RX_WEATHER_EXPORT RX_DSO_EXPORT
#else
#define RX_WEATHER_EXPORT RX_DSO_IMPORT
#endif

#endif  // RX_WEATHER_EXPORT_H_
