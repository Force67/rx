#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "base/containers/unordered_map.h"
#include "core/file_system.h"
#include "core/text_reader.h"
#include "core/text_writer.h"
#include "render/core/settings_ini.h"

#include <ctype.h>
#include <math.h>

namespace rx::render {
namespace {

base::String Trim(base::StringRef sv) {
  size_t b = 0, e = sv.size();
  while (b < e && ::isspace(static_cast<unsigned char>(sv[b]))) ++b;
  while (e > b && ::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
  return base::String(sv.substr(b, e - b));
}

base::String Lower(base::String s) {
  for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  return s;
}

// enum <-> name

const char* Name(AntiAliasingMode m) {
  switch (m) {
    case AntiAliasingMode::kNone: return "none";
    case AntiAliasingMode::kTaa: return "taa";
    case AntiAliasingMode::kUpscaler: return "upscaler";
    case AntiAliasingMode::kMsaa: return "msaa";
  }
  return "taa";
}
bool Parse(const base::String& v, AntiAliasingMode& out) {
  if (v == "none") out = AntiAliasingMode::kNone;
  else if (v == "taa") out = AntiAliasingMode::kTaa;
  else if (v == "upscaler") out = AntiAliasingMode::kUpscaler;
  else if (v == "msaa") out = AntiAliasingMode::kMsaa;
  else return false;
  return true;
}

const char* Name(UpscalerKind k) {
  switch (k) {
    case UpscalerKind::kNone: return "none";
    case UpscalerKind::kFsr3: return "fsr3";
    case UpscalerKind::kDlss: return "dlss";
    case UpscalerKind::kXess: return "xess";
  }
  return "none";
}
bool Parse(const base::String& v, UpscalerKind& out) {
  if (v == "none") out = UpscalerKind::kNone;
  else if (v == "fsr3" || v == "fsr") out = UpscalerKind::kFsr3;
  else if (v == "dlss") out = UpscalerKind::kDlss;
  else if (v == "xess") out = UpscalerKind::kXess;
  else return false;
  return true;
}

const char* Name(UpscalerQuality q) {
  switch (q) {
    case UpscalerQuality::kNativeAa: return "native";
    case UpscalerQuality::kQuality: return "quality";
    case UpscalerQuality::kBalanced: return "balanced";
    case UpscalerQuality::kPerformance: return "performance";
  }
  return "quality";
}
bool Parse(const base::String& v, UpscalerQuality& out) {
  if (v == "native" || v == "nativeaa" || v == "dlaa") out = UpscalerQuality::kNativeAa;
  else if (v == "quality") out = UpscalerQuality::kQuality;
  else if (v == "balanced") out = UpscalerQuality::kBalanced;
  else if (v == "performance") out = UpscalerQuality::kPerformance;
  else return false;
  return true;
}

const char* Name(HumanQualityTier t) {
  switch (t) {
    case HumanQualityTier::kHero: return "hero";
    case HumanQualityTier::kStandard: return "standard";
    case HumanQualityTier::kDistant: return "distant";
  }
  return "hero";
}

bool Parse(const base::String& v, HumanQualityTier& out) {
  if (v == "hero") out = HumanQualityTier::kHero;
  else if (v == "standard") out = HumanQualityTier::kStandard;
  else if (v == "distant") out = HumanQualityTier::kDistant;
  else return false;
  return true;
}

const char* Name(TonemapOperator t) {
  switch (t) {
    case TonemapOperator::kAces: return "aces";
    case TonemapOperator::kReinhard: return "reinhard";
    case TonemapOperator::kNone: return "none";
    case TonemapOperator::kAgx: return "agx";
  }
  return "aces";
}
bool Parse(const base::String& v, TonemapOperator& out) {
  if (v == "aces") out = TonemapOperator::kAces;
  else if (v == "reinhard") out = TonemapOperator::kReinhard;
  else if (v == "none") out = TonemapOperator::kNone;
  else if (v == "agx") out = TonemapOperator::kAgx;
  else return false;
  return true;
}

const char* Bool(bool b) { return b ? "true" : "false"; }

}  // namespace

base::String SettingsToIni(const RenderSettings& s) {
  TextWriter o;
  o << "# Rx render preset. Editable. Load/save it from the debug ui\n"
    << "# (Renderer panel -> Platform preset). Unlisted keys keep their value.\n\n";

  o << "[antialiasing]\n";
  o << "aa_mode = " << Name(s.aa_mode) << "\n";
  o << "upscaler = " << Name(s.upscaler) << "\n";
  o << "upscaler_quality = " << Name(s.upscaler_quality) << "\n";
  o << "sharpness = " << s.sharpness << "\n";
  o << "taa_history_blend = " << s.taa_history_blend << "\n\n";

  o << "[resolution]\n";
  o << "render_scale = " << s.render_scale << "\n";
  o << "dynamic_resolution = " << Bool(s.dynamic_resolution) << "\n";
  o << "dynamic_target_ms = " << s.dynamic_target_ms << "\n";
  o << "dynamic_min_scale = " << s.dynamic_min_scale << "\n\n";

  o << "[shadows]\n";
  o << "rt_shadows = " << Bool(s.rt_shadows) << "\n";
  o << "sun_angular_radius = " << s.sun_angular_radius << "\n";
  o << "shadow_maps = " << Bool(s.shadow_maps) << "\n";
  o << "shadow_resolution = " << s.shadow_resolution << "\n";
  o << "shadow_distance = " << s.shadow_distance << "\n\n";

  o << "[geometry]\n";
  o << "gpu_culling = " << Bool(s.gpu_culling) << "\n";
  o << "gpu_occlusion = " << Bool(s.gpu_occlusion) << "\n";
  o << "distance_lod = " << Bool(s.distance_lod) << "\n";
  o << "mesh_shader_lod = " << Bool(s.mesh_shader_lod) << "\n";
  o << "procedural_grass = " << Bool(s.procedural_grass) << "\n";
  o << "vsync = " << Bool(s.vsync) << "\n\n";

  o << "[sky]\n";
  o << "sky = " << Bool(s.sky) << "\n";
  o << "ibl = " << Bool(s.ibl) << "\n";
  o << "ibl_intensity = " << s.ibl_intensity << "\n";
  o << "aerial_perspective = " << s.aerial_perspective << "\n";
  o << "clouds = " << Bool(s.clouds) << "\n";
  o << "cloud_coverage = " << s.cloud_coverage << "\n";
  o << "cloudscape = " << Bool(s.cloudscape) << "\n";
  o << "cloudscape_steps = " << s.cloudscape_steps << "\n\n";

  o << "[weather]\n";
  o << "precipitation = " << s.weather.precipitation << "\n";
  o << "precip_snow = " << Bool(s.weather.snow) << "\n";
  o << "volumetric = " << Bool(s.weather.volumetric) << "\n";
  o << "wind_speed = " << s.weather.wind_speed << "\n";
  o << "wind_yaw = " << s.weather.wind_yaw << "\n";
  o << "gustiness = " << s.weather.gustiness << "\n";
  o << "wetness = " << s.weather.wetness << "\n";
  o << "snow_cover = " << s.weather.snow_cover << "\n";
  o << "lightning = " << s.weather.lightning << "\n";
  // Not "rt_shadows": the parser ignores sections and that key is already the
  // [shadows] ray-traced sun toggle.
  o << "precip_rt_shadows = " << Bool(s.weather.rt_shadows) << "\n";
  o << "aurora = " << Bool(s.weather.aurora) << "\n";
  o << "aurora_intensity = " << s.weather.aurora_intensity << "\n\n";

  o << "[ambient_occlusion]\n";
  o << "rtao = " << Bool(s.rtao) << "\n";
  o << "ssao = " << Bool(s.ssao) << "\n";
  o << "ao_radius = " << s.ao_radius << "\n";
  o << "ao_intensity = " << s.ao_intensity << "\n";
  o << "ao_rays = " << s.ao_rays << "\n\n";

  o << "[global_illumination]\n";
  o << "ddgi = " << Bool(s.ddgi) << "\n";
  o << "ddgi_spacing = " << s.ddgi_spacing << "\n";
  o << "ddgi_intensity = " << s.ddgi_intensity << "\n";
  o << "ssgi = " << Bool(s.ssgi) << "\n";
  o << "rcgi = " << Bool(s.rcgi) << "\n\n";

  o << "[reflections]\n";
  o << "rt_reflections = " << Bool(s.rt_reflections) << "\n";
  o << "reflection_roughness_cutoff = " << s.reflection_roughness_cutoff << "\n";
  o << "water_reflections = " << Bool(s.water_reflections) << "\n";
  o << "ssr = " << Bool(s.ssr) << "\n\n";

  o << "[water]\n";
  o << "adaptive_water = " << Bool(s.adaptive_water) << "\n";
  o << "water_triangle_budget = " << s.water_triangle_budget << "\n";
  o << "water_target_triangle_pixels = " << s.water_target_triangle_pixels << "\n";
  o << "water_field = " << Bool(s.water_field) << "\n";
  o << "fluid_sim = " << Bool(s.fluid_sim) << "\n";
  o << "water_interaction = " << Bool(s.water_interaction) << "\n";
  o << "shore_wetting = " << Bool(s.shore_wetting) << "\n";
  o << "shore_drying_time = " << s.shore_drying_time << "\n";
  o << "water_absorption_r = " << s.water_absorption[0] << "\n";
  o << "water_absorption_g = " << s.water_absorption[1] << "\n";
  o << "water_absorption_b = " << s.water_absorption[2] << "\n";
  o << "water_absorption_scale = " << s.water_absorption_scale << "\n";
  o << "water_transmission = " << s.water_transmission << "\n";
  o << "water_refl_foam_gain = " << s.water_refl_foam_gain << "\n";
  o << "human_tier_cap = " << Name(s.human_tier_cap) << "\n";
  o << "hair_transmittance = " << s.hair_transmittance << "\n";
  o << "hair_transmittance_depth = " << s.hair_transmittance_depth << "\n";
  o << "hair_fibre_scale = " << s.hair_fibre_scale << "\n";
  o << "hair_shadow_density = " << s.hair_shadow_density << "\n";
  o << "water_sss_intensity = " << s.water_sss_intensity << "\n";
  o << "water_sss_exponent = " << s.water_sss_exponent << "\n";
  o << "water_caustics = " << Bool(s.water_caustics) << "\n";
  o << "water_rest_height = " << s.water_rest_height << "\n";
  o << "water_caustic_intensity = " << s.water_caustic_intensity << "\n";
  o << "water_caustic_depth_fade = " << s.water_caustic_depth_fade << "\n";
  o << "water_caustic_receiver_depth = " << s.water_caustic_receiver_depth << "\n\n";

  o << "[path_tracing]\n";
  o << "path_trace = " << Bool(s.path_trace) << "\n";
  o << "path_trace_reference = " << Bool(s.path_trace_reference) << "\n";
  o << "path_trace_spp = " << s.path_trace_spp << "\n";
  o << "path_trace_accum = " << s.path_trace_accum << "\n";
  o << "path_trace_recon = " << Bool(s.path_trace_recon) << "\n";
  o << "path_trace_recon_weight = " << s.path_trace_recon_weight << "\n";
  o << "path_trace_recon_atrous = " << s.path_trace_recon_atrous << "\n\n";

  o << "[fog]\n";
  o << "fog = " << Bool(s.fog) << "\n";
  o << "fog_density = " << s.fog_density << "\n";
  o << "fog_height_falloff = " << s.fog_height_falloff << "\n";
  o << "fog_base_height = " << s.fog_base_height << "\n";
  o << "fog_anisotropy = " << s.fog_anisotropy << "\n";
  o << "froxel_fog = " << Bool(s.froxel_fog) << "\n";
  o << "froxel_density = " << s.froxel_density << "\n\n";

  o << "[post]\n";
  o << "bloom = " << Bool(s.bloom) << "\n";
  o << "bloom_intensity = " << s.bloom_intensity << "\n";
  o << "motion_blur = " << Bool(s.motion_blur) << "\n";
  o << "sss = " << Bool(s.sss) << "\n";
  o << "auto_exposure = " << Bool(s.auto_exposure) << "\n";
  o << "adaptation_speed = " << s.adaptation_speed << "\n";
  o << "exposure = " << s.exposure << "\n";
  o << "tonemap = " << Name(s.tonemap) << "\n";
  return o.Take();
}

int ApplyIni(base::StringRef text, RenderSettings& s) {
  // Collect "key = value" pairs (lowercased keys), ignoring sections/comments.
  base::UnorderedMap<base::String, base::String> kv;
  LineReader lines(text);
  base::StringRef piece;
  while (lines.Next(&piece)) {
    base::String line(piece.data(), piece.size());
    if (auto hash = line.find_first_of(";#"); hash != base::String::npos) line.resize(hash);
    base::String t = Trim(line);
    if (t.empty() || t[0] == '[') continue;
    auto eq = t.find('=');
    if (eq == base::String::npos) continue;
    kv[Lower(Trim(base::StringRef(t).substr(0, eq)))] = Trim(base::StringRef(t).substr(eq + 1));
  }
  if (kv.empty()) return 0;

  int applied = 0;
  auto take = [&](const char* key, auto&& fn) {
    const base::String* value = kv.find(key);
    if (value && fn(*value)) ++applied;
  };
  auto as_bool = [](const base::String& v, bool& out) {
    const base::String l = Lower(v);
    if (l == "true" || l == "1" || l == "on" || l == "yes") { out = true; return true; }
    if (l == "false" || l == "0" || l == "off" || l == "no") { out = false; return true; }
    return false;
  };
  auto as_f32 = [](const base::String& v, f32& out) {
    f32 parsed = 0.0f;
    if (!ParseWholeF32(v, &parsed) || !isfinite(parsed)) return false;
    out = parsed;
    return true;
  };
  auto as_u32 = [](const base::String& v, u32& out) {
    if (v.empty() || v[0] == '-') return false;
    u32 parsed = 0;
    if (!ParseWholeU32(v, &parsed)) return false;
    out = parsed;
    return true;
  };

  auto b = [&](const char* k, bool& f) { take(k, [&](const base::String& v) { return as_bool(v, f); }); };
  auto fl = [&](const char* k, f32& f) { take(k, [&](const base::String& v) { return as_f32(v, f); }); };
  auto u = [&](const char* k, u32& f) { take(k, [&](const base::String& v) { return as_u32(v, f); }); };
  auto en = [&](const char* k, auto& f) {
    take(k, [&](const base::String& v) { return Parse(Lower(v), f); });
  };

  en("aa_mode", s.aa_mode);
  en("upscaler", s.upscaler);
  en("upscaler_quality", s.upscaler_quality);
  fl("sharpness", s.sharpness);
  fl("taa_history_blend", s.taa_history_blend);

  fl("render_scale", s.render_scale);
  b("dynamic_resolution", s.dynamic_resolution);
  fl("dynamic_target_ms", s.dynamic_target_ms);
  fl("dynamic_min_scale", s.dynamic_min_scale);

  b("rt_shadows", s.rt_shadows);
  fl("sun_angular_radius", s.sun_angular_radius);
  b("shadow_maps", s.shadow_maps);
  u("shadow_resolution", s.shadow_resolution);
  fl("shadow_distance", s.shadow_distance);

  b("gpu_culling", s.gpu_culling);
  b("gpu_occlusion", s.gpu_occlusion);
  b("distance_lod", s.distance_lod);
  b("mesh_shader_lod", s.mesh_shader_lod);
  b("procedural_grass", s.procedural_grass);
  b("vsync", s.vsync);

  b("sky", s.sky);
  b("ibl", s.ibl);
  fl("ibl_intensity", s.ibl_intensity);
  fl("aerial_perspective", s.aerial_perspective);
  b("clouds", s.clouds);
  fl("cloud_coverage", s.cloud_coverage);
  b("cloudscape", s.cloudscape);
  u("cloudscape_steps", s.cloudscape_steps);

  fl("precipitation", s.weather.precipitation);
  b("precip_snow", s.weather.snow);
  b("volumetric", s.weather.volumetric);
  fl("wind_speed", s.weather.wind_speed);
  fl("wind_yaw", s.weather.wind_yaw);
  fl("gustiness", s.weather.gustiness);
  fl("wetness", s.weather.wetness);
  fl("snow_cover", s.weather.snow_cover);
  fl("lightning", s.weather.lightning);
  b("precip_rt_shadows", s.weather.rt_shadows);
  b("aurora", s.weather.aurora);
  fl("aurora_intensity", s.weather.aurora_intensity);

  b("rtao", s.rtao);
  b("ssao", s.ssao);
  fl("ao_radius", s.ao_radius);
  fl("ao_intensity", s.ao_intensity);
  u("ao_rays", s.ao_rays);

  b("ddgi", s.ddgi);
  fl("ddgi_spacing", s.ddgi_spacing);
  fl("ddgi_intensity", s.ddgi_intensity);
  b("ssgi", s.ssgi);
  b("rcgi", s.rcgi);

  b("rt_reflections", s.rt_reflections);
  fl("reflection_roughness_cutoff", s.reflection_roughness_cutoff);
  b("water_reflections", s.water_reflections);
  b("ssr", s.ssr);
  b("adaptive_water", s.adaptive_water);
  u("water_triangle_budget", s.water_triangle_budget);
  fl("water_target_triangle_pixels", s.water_target_triangle_pixels);
  b("water_field", s.water_field);
  b("fluid_sim", s.fluid_sim);
  b("water_interaction", s.water_interaction);
  b("shore_wetting", s.shore_wetting);
  fl("shore_drying_time", s.shore_drying_time);
  fl("water_absorption_r", s.water_absorption[0]);
  fl("water_absorption_g", s.water_absorption[1]);
  fl("water_absorption_b", s.water_absorption[2]);
  fl("water_absorption_scale", s.water_absorption_scale);
  fl("water_transmission", s.water_transmission);
  fl("water_refl_foam_gain", s.water_refl_foam_gain);
  en("human_tier_cap", s.human_tier_cap);
  b("hair_transmittance", s.hair_transmittance);
  fl("hair_transmittance_depth", s.hair_transmittance_depth);
  fl("hair_fibre_scale", s.hair_fibre_scale);
  fl("hair_shadow_density", s.hair_shadow_density);
  fl("water_sss_intensity", s.water_sss_intensity);
  fl("water_sss_exponent", s.water_sss_exponent);
  b("water_caustics", s.water_caustics);
  fl("water_rest_height", s.water_rest_height);
  fl("water_caustic_intensity", s.water_caustic_intensity);
  fl("water_caustic_depth_fade", s.water_caustic_depth_fade);
  fl("water_caustic_receiver_depth", s.water_caustic_receiver_depth);

  b("path_trace", s.path_trace);
  b("path_trace_reference", s.path_trace_reference);
  u("path_trace_spp", s.path_trace_spp);
  u("path_trace_accum", s.path_trace_accum);
  b("path_trace_recon", s.path_trace_recon);
  fl("path_trace_recon_weight", s.path_trace_recon_weight);
  u("path_trace_recon_atrous", s.path_trace_recon_atrous);

  b("fog", s.fog);
  fl("fog_density", s.fog_density);
  fl("fog_height_falloff", s.fog_height_falloff);
  fl("fog_base_height", s.fog_base_height);
  fl("fog_anisotropy", s.fog_anisotropy);
  b("froxel_fog", s.froxel_fog);
  fl("froxel_density", s.froxel_density);

  b("bloom", s.bloom);
  fl("bloom_intensity", s.bloom_intensity);
  b("motion_blur", s.motion_blur);
  b("sss", s.sss);
  b("auto_exposure", s.auto_exposure);
  fl("adaptation_speed", s.adaptation_speed);
  fl("exposure", s.exposure);
  en("tonemap", s.tonemap);
  return applied;
}

bool LoadSettingsIni(base::StringRef path, RenderSettings& s) {
  base::String text;
  if (!fs::ReadTextFile(path, &text)) return false;
  ApplyIni(text, s);
  return true;
}

bool SaveSettingsIni(base::StringRef path, const RenderSettings& s) {
  return fs::WriteTextFile(path, SettingsToIni(s));
}

}  // namespace rx::render
