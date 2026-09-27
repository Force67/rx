#ifndef RX_RENDER_PRESETS_H_
#define RX_RENDER_PRESETS_H_


#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/export.h"
#include "core/types.h"
#include "render/rhi/device.h"
#include "render/core/settings.h"

namespace rx::render {

// Hardware quality tiers. kAuto resolves to one of the concrete tiers from the
// device caps at startup; the rest can be forced with --preset or the debug ui.
// Ordered low to high so comparisons (tier >= kConsole) read naturally. Each
// tier is the ini file engine/render/presets/<PresetName>.ini, embedded.
enum class QualityPreset : u8 {
  kAuto,
  kAndroidLow,     // old or entry-level mobile gpus
  kAndroidMedium,  // mid-range mobile, and any mobile gpu we do not recognize
  kAndroidHigh,    // flagship mobile (Adreno 730+, Immortalis, Xclipse)
  kSteamDeck,      // rdna2 handheld, ray query at a tight power budget
  kLowEnd,     // old or weak discrete gpus, no ray tracing
  kConsole,    // ps5/series-x class, full rt tuned for 60 fps
  kMedium,     // mid-range rt desktop (rtx 3060 / rx 6600)
  kHigh,       // high-end rt desktop (rtx 4070/4080)
  kUltra,      // flagship (rtx 4090/5090), everything on
  kAndroid = kAndroidMedium,  // the name before the android tiers split
};

// RenderSettings defaults overlaid with the tier's ini, then every ray-traced
// feature clamped off when the device cannot do it, so the result is always
// runnable. The clamps are the only policy here; the tiers are the files.
RX_RENDER_EXPORT RenderSettings PresetSettings(QualityPreset preset, const DeviceCaps& caps);

// Picks a concrete tier from the gpu class, vram and ray tracing support.
RX_RENDER_EXPORT QualityPreset DetectPreset(const DeviceCaps& caps);

// Resolves kAuto, leaves a concrete tier untouched.
inline QualityPreset ResolvePreset(QualityPreset preset, const DeviceCaps& caps) {
  return preset == QualityPreset::kAuto ? DetectPreset(caps) : preset;
}

RX_RENDER_EXPORT const char* PresetName(QualityPreset preset);

// The embedded ini text of a tier, by PresetName ("medium"), for resolving an
// `include = medium` line. False for a name that is no tier.
RX_RENDER_EXPORT bool PresetIni(base::StringRef name, base::String* text);
RX_RENDER_EXPORT QualityPreset ParsePreset(const base::String& name);  // kAuto on no match

}  // namespace rx::render

#endif  // RX_RENDER_PRESETS_H_
