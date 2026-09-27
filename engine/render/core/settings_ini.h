#ifndef RX_RENDER_SETTINGS_INI_H_
#define RX_RENDER_SETTINGS_INI_H_

#include "base/functional/function.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/export.h"
#include "render/core/settings.h"

namespace rx::render {

// Text (de)serialization of RenderSettings as a flat INI: "key = value" lines
// grouped under cosmetic [section] headers, enums written as lowercase names.
// The quality tiers are these files (engine/render/presets/<tier>.ini, embedded
// at build time); the debug ui loads and saves the same format.
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

// Finds the text of the ini an `include = <name>` line names. False when there
// is no such file.
using IniResolver = base::Function<bool(base::StringRef name, base::String* text)>;

// Overlays recognized "key = value" lines from `text` onto `s`. Section headers,
// blank lines and ; / # comments are ignored; unknown keys are skipped.
// `include = <name>` lines apply the named ini first, through `resolve`
// (recursively, at most 8 deep), so the including file's own keys win; an
// include that does not resolve is logged and skipped. Returns the number of
// this file's lines applied, resolved includes counted as one each.
RX_RENDER_EXPORT int ApplyIni(base::StringRef text, RenderSettings& s,
                              const IniResolver& resolve = {});

// Reads a preset file and overlays it onto `s` (see ApplyIni), resolving
// includes to <name>.ini beside it. False if the file cannot be opened.
RX_RENDER_EXPORT bool LoadSettingsIni(base::StringRef path, RenderSettings& s);

// Writes SettingsToIni(s) to `path`. False on a write error.
RX_RENDER_EXPORT bool SaveSettingsIni(base::StringRef path, const RenderSettings& s);

}  // namespace rx::render

#endif  // RX_RENDER_SETTINGS_INI_H_
