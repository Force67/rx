#ifndef RX_RENDER_SETTINGS_INI_H_
#define RX_RENDER_SETTINGS_INI_H_

#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "render/core/settings.h"

namespace rx::render {

// Text (de)serialization of RenderSettings as a flat INI: "key = value" lines
// grouped under cosmetic [section] headers, enums written as lowercase names.
// The quality tiers carry these keys in the [render.<group>] sections of their
// platform config files (rx/config/<tier>.ini); the debug ui loads and saves the
// same format.
//
// The persistent quality/performance knobs and the [weather] group (so a saved
// file round-trips a weather look for testing) are covered. Scene state owned
// by other systems is deliberately excluded so a preset never fights them:
//   - sun_direction / sun_intensity / sun_color / ambient  (day/night clock)
//   - weather strike_* fields                (transient per-strike lightning)
//   - color_grade                                           (artistic / env)
//   - debug_view / wireframe / path_trace_recon_debug       (debug overlays)
// ApplyIni leaves any field whose key is absent untouched, so partial files and
// the excluded fields keep their incoming value.

// Serializes the covered fields of `s` to INI text.
RX_RENDER_EXPORT base::String SettingsToIni(const RenderSettings& s);

// Overlays recognized "key = value" lines from `text` onto `s`. Section headers,
// blank lines and ; / # comments are ignored; unknown keys are skipped. Returns
// the number of keys applied. Platform config files (app/platform_config.h)
// carry these keys in their [render.<group>] sections and handle includes.
RX_RENDER_EXPORT int ApplyIni(base::StringRef text, RenderSettings& s);

// Reads a file and overlays its keys onto `s` (see ApplyIni). False if the file
// cannot be opened.
RX_RENDER_EXPORT bool LoadSettingsIni(base::StringRef path, RenderSettings& s);

// Writes SettingsToIni(s) to `path`. False on a write error.
RX_RENDER_EXPORT bool SaveSettingsIni(base::StringRef path, const RenderSettings& s);

}  // namespace rx::render

#endif  // RX_RENDER_SETTINGS_INI_H_
