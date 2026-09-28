#include "base/strings/xstring.h"
#include "rxe/render/core/presets.h"

#include "foundation/system/platform.h"
#include "rxe/render/core/settings_ini.h"

#include <ctype.h>
#include <string.h>
#include <initializer_list>



namespace rx::render {
namespace {

base::String Lower(base::String s) {
  for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool Contains(const base::String& s, const char* marker) {
  return s.find(marker) != base::String::npos;
}

bool IsMobileGpu(const base::String& name) {
  const base::String n = Lower(name);
  for (const char* marker :
       {"adreno", "mali", "powervr", "apple", "xclipse", "immortalis", "vivante"}) {
    if (Contains(n, marker)) return true;
  }
  return false;
}

// The model number after `marker` ("adreno (tm) 740" -> 740), 0 when absent.
u32 ModelNumber(const base::String& n, const char* marker) {
  size_t i = n.find(marker);
  if (i == base::String::npos) return 0;
  i += ::strlen(marker);
  while (i < n.size() && !::isdigit(static_cast<unsigned char>(n[i]))) ++i;
  u32 number = 0;
  while (i < n.size() && ::isdigit(static_cast<unsigned char>(n[i])))
    number = number * 10 + static_cast<u32>(n[i++] - '0');
  return number;
}

// Mobile gpus by generation, from the adapter name. A recognized flagship gets
// the high tier and a recognized old or entry part the low one; a name we do
// not know lands in the middle rather than guessing either way.
QualityPreset DetectAndroidTier(const base::String& name) {
  const base::String n = Lower(name);
  if (Contains(n, "immortalis") || Contains(n, "xclipse") || Contains(n, "apple"))
    return QualityPreset::kAndroidHigh;
  if (const u32 adreno = ModelNumber(n, "adreno")) {
    if (adreno >= 730) return QualityPreset::kAndroidHigh;  // 8 gen 2 and newer
    return adreno >= 640 ? QualityPreset::kAndroidMedium : QualityPreset::kAndroidLow;
  }
  if (const u32 mali = ModelNumber(n, "mali-g")) {
    // Three digits since Valhall gen 2 (G710); two before (G57..G78).
    if (mali >= 100) {
      if (mali >= 700) return QualityPreset::kAndroidHigh;
      return mali >= 600 ? QualityPreset::kAndroidMedium : QualityPreset::kAndroidLow;
    }
    return mali >= 70 ? QualityPreset::kAndroidMedium : QualityPreset::kAndroidLow;
  }
  if (Contains(n, "mali-t") || Contains(n, "powervr") || Contains(n, "vivante"))
    return QualityPreset::kAndroidLow;
  return QualityPreset::kAndroidMedium;
}

}  // namespace

RenderSettings PresetSettings(base::StringRef tier_ini, const DeviceCaps& caps) {
  RenderSettings s;
  ApplyIni(tier_ini, s);

  // Clamp to what the device can actually run so a forced tier never hangs.
  if (!caps.ray_query) {
    s.rt_shadows = false;
    s.rtao = false;
    s.ddgi = false;
    s.rcgi = false;  // needs ray query (or a startup-seeded SDF software path)
    s.rt_reflections = false;
    s.water_reflections = false;
    s.fog = false;
    s.path_trace = false;
  }

  // Cascaded shadow maps are the sun-shadow path whenever ray tracing isn't, so
  // every non-rt tier (and forced-low on capable gpus) still casts sun shadows.
  if (!s.rt_shadows) s.shadow_maps = true;
  // The tier files name dlss where nvidia can run it; everyone else gets fsr3.
  // The renderer falls back to taa if the backend is not compiled in either.
  if (s.upscaler == UpscalerKind::kDlss && !Contains(Lower(caps.adapter_name), "nvidia"))
    s.upscaler = UpscalerKind::kFsr3;
  if (s.aa_mode == AntiAliasingMode::kUpscaler && s.upscaler == UpscalerKind::kNone)
    s.aa_mode = AntiAliasingMode::kTaa;
  return s;
}

QualityPreset DetectPreset(const DeviceCaps& caps) {
  // Known from the board rather than guessed from the gpu class, which is all
  // the integrated fallback below can do for other handhelds.
  if (IsSteamDeck()) return QualityPreset::kSteamDeck;
  // Before the ray query split: current mobile flagships have ray query too. On
  // Android every gpu is a mobile one, named in our list or not.
#if defined(__ANDROID__)
  return DetectAndroidTier(caps.adapter_name);
#endif
  if (IsMobileGpu(caps.adapter_name)) return DetectAndroidTier(caps.adapter_name);
  if (!caps.ray_query) return QualityPreset::kLowEnd;
  if (caps.integrated) return QualityPreset::kSteamDeck;

  const u64 gib = caps.device_local_bytes >> 30;
  if (gib >= 11) return QualityPreset::kUltra;  // 12 GB+ flagship
  if (gib >= 7) return QualityPreset::kHigh;     // ~8 GB
  return QualityPreset::kMedium;
}

const char* PresetName(QualityPreset preset) {
  switch (preset) {
    case QualityPreset::kAuto: return "auto";
    case QualityPreset::kAndroidLow: return "android_low";
    case QualityPreset::kAndroidMedium: return "android_medium";
    case QualityPreset::kAndroidHigh: return "android_high";
    case QualityPreset::kSteamDeck: return "steamdeck";
    case QualityPreset::kLowEnd: return "low";
    case QualityPreset::kConsole: return "console";
    case QualityPreset::kMedium: return "medium";
    case QualityPreset::kHigh: return "high";
    case QualityPreset::kUltra: return "ultra";
  }
  return "auto";
}

QualityPreset ParsePreset(const base::String& name) {
  const base::String n = Lower(name);
  if (n == "android_low") return QualityPreset::kAndroidLow;
  if (n == "android_medium" || n == "android" || n == "mobile")
    return QualityPreset::kAndroidMedium;
  if (n == "android_high") return QualityPreset::kAndroidHigh;
  if (n == "steamdeck" || n == "deck") return QualityPreset::kSteamDeck;
  if (n == "low" || n == "lowend") return QualityPreset::kLowEnd;
  if (n == "console") return QualityPreset::kConsole;
  if (n == "medium" || n == "mid") return QualityPreset::kMedium;
  if (n == "high") return QualityPreset::kHigh;
  if (n == "ultra") return QualityPreset::kUltra;
  return QualityPreset::kAuto;
}

}  // namespace rx::render
