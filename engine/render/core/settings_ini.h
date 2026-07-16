#ifndef RX_RENDER_SETTINGS_INI_H_
#define RX_RENDER_SETTINGS_INI_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "core/export.h"
#include "render/core/settings.h"

namespace rx::render {

// Text (de)serialization of RenderSettings as a flat INI: "key = value" lines
// grouped under cosmetic [section] headers, enums written as lowercase names.
// This backs the editable platform presets in engine/render/presets and the
// load/save controls in the debug ui.
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
std::string SettingsToIni(const RenderSettings& s);

// Overlays recognized "key = value" lines from `text` onto `s`. Section headers,
// blank lines and ; / # comments are ignored; unknown keys are skipped. Returns
// the number of keys applied.
int ApplyIni(std::string_view text, RenderSettings& s);

// Reads a preset file and overlays it onto `s` (see ApplyIni). False if the file
// cannot be opened.
RX_RENDER_EXPORT bool LoadSettingsIni(const std::filesystem::path& path, RenderSettings& s);

// Writes SettingsToIni(s) to `path`. False on a write error.
RX_RENDER_EXPORT bool SaveSettingsIni(const std::filesystem::path& path, const RenderSettings& s);

}  // namespace rx::render

#endif  // RX_RENDER_SETTINGS_INI_H_
