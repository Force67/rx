#include "asset/materialx.h"

#include <fstream>
#include <sstream>

#include "core/log.h"

namespace rx::asset {
namespace {

// Value of an attribute key="..." inside a single tag's text.
std::string Attr(const std::string& tag, const char* key) {
  std::string pat = std::string(key) + "=\"";
  size_t p = tag.find(pat);
  if (p == std::string::npos) return "";
  p += pat.size();
  size_t e = tag.find('"', p);
  if (e == std::string::npos) return "";
  return tag.substr(p, e - p);
}

// MaterialX vector/color values are comma separated; parse up to n floats.
void ParseFloats(const std::string& value, f32* out, int n) {
  std::string s = value;
  for (char& c : s) {
    if (c == ',') c = ' ';
  }
  std::istringstream ss(s);
  f32 v;
  for (int i = 0; i < n && (ss >> v); ++i) out[i] = v;
}

// Calls fn(name, value) for every <input> tag carrying a constant value.
// Inputs driven by a node graph have no value attribute and are skipped, so a
// textured material keeps whatever the caller seeded as the default.
template <typename Fn>
void ForEachConstantInput(const std::string& doc, Fn fn) {
  size_t p = 0;
  while ((p = doc.find("<input", p)) != std::string::npos) {
    size_t e = doc.find('>', p);
    if (e == std::string::npos) break;
    std::string tag = doc.substr(p, e - p);
    p = e + 1;
    std::string name = Attr(tag, "name");
    std::string value = Attr(tag, "value");
    if (name.empty() || value.empty()) continue;
    fn(name, value);
  }
}

// Autodesk Standard Surface (standard_surface). Predates OpenPBR and uses a
// different input vocabulary: `base`/`metalness`/`coat`/`sheen` where OpenPBR
// says `base_weight`/`base_metalness`/`coat_weight`/`fuzz_weight`.
void ParseStandardSurface(const std::string& doc, Material* out) {
  f32 base_weight = 1.0f;
  f32 sheen_weight = 0.0f, sheen_color[3] = {1, 1, 1};
  f32 emission_weight = 0.0f, emission_color[3] = {1, 1, 1};
  f32 thin_film_thickness = 0.0f;

  ForEachConstantInput(doc, [&](const std::string& name, const std::string& value) {
    if (name == "base") {
      ParseFloats(value, &base_weight, 1);
    } else if (name == "base_color") {
      ParseFloats(value, out->base_color_factor, 3);
    } else if (name == "metalness") {
      ParseFloats(value, &out->metallic_factor, 1);
    } else if (name == "specular_roughness") {
      ParseFloats(value, &out->roughness_factor, 1);
    } else if (name == "specular_IOR") {
      ParseFloats(value, &out->ior, 1);
    } else if (name == "specular_anisotropy") {
      ParseFloats(value, &out->anisotropy, 1);
    } else if (name == "coat") {
      ParseFloats(value, &out->clearcoat, 1);
    } else if (name == "coat_roughness") {
      ParseFloats(value, &out->clearcoat_roughness, 1);
    } else if (name == "sheen") {
      ParseFloats(value, &sheen_weight, 1);
    } else if (name == "sheen_color") {
      ParseFloats(value, sheen_color, 3);
    } else if (name == "sheen_roughness") {
      ParseFloats(value, &out->sheen_roughness, 1);
    } else if (name == "subsurface") {
      ParseFloats(value, &out->subsurface, 1);
    } else if (name == "subsurface_color") {
      ParseFloats(value, out->subsurface_color, 3);
    } else if (name == "transmission") {
      ParseFloats(value, &out->transmission, 1);
    } else if (name == "emission") {
      ParseFloats(value, &emission_weight, 1);
    } else if (name == "emission_color") {
      ParseFloats(value, emission_color, 3);
    } else if (name == "thin_film_thickness") {
      ParseFloats(value, &thin_film_thickness, 1);
    }
  });

  for (int i = 0; i < 3; ++i) {
    out->base_color_factor[i] *= base_weight;
    out->sheen_color[i] = sheen_color[i] * sheen_weight;
    out->emissive_factor[i] = emission_color[i] * emission_weight;
  }
  // standard_surface states thin_film_thickness in nanometres directly, unlike
  // OpenPBR, which uses micrometres.
  if (thin_film_thickness > 0.0f) {
    out->iridescence = 1.0f;
    out->iridescence_thickness = thin_film_thickness;
  }
}

// OpenPBR Surface (open_pbr_surface), the ASWF standard. The caller has already
// seeded the spec defaults, so anything this document does not author keeps the
// OpenPBR value rather than the engine's glTF-derived one.
void ParseOpenPbrSurface(const std::string& doc, Material* out) {
  f32 base_weight = 1.0f;
  f32 fuzz_weight = 0.0f, fuzz_color[3] = {1, 1, 1};
  f32 emission_luminance = 0.0f, emission_color[3] = {1, 1, 1};
  f32 thin_film_thickness_um = 0.5f;
  f32 anisotropy = 0.0f;
  f32 opacity = 1.0f;

  ForEachConstantInput(doc, [&](const std::string& name, const std::string& value) {
    if (name == "base_weight") {
      ParseFloats(value, &base_weight, 1);
    } else if (name == "base_color") {
      ParseFloats(value, out->base_color_factor, 3);
    } else if (name == "base_diffuse_roughness") {
      ParseFloats(value, &out->base_diffuse_roughness, 1);
    } else if (name == "base_metalness") {
      ParseFloats(value, &out->metallic_factor, 1);
    } else if (name == "specular_weight") {
      ParseFloats(value, &out->specular_weight, 1);
    } else if (name == "specular_color") {
      ParseFloats(value, out->specular_color, 3);
    } else if (name == "specular_roughness") {
      ParseFloats(value, &out->roughness_factor, 1);
    } else if (name == "specular_ior") {
      ParseFloats(value, &out->ior, 1);
    } else if (name == "specular_roughness_anisotropy") {
      ParseFloats(value, &anisotropy, 1);
    } else if (name == "transmission_weight") {
      ParseFloats(value, &out->transmission, 1);
    } else if (name == "subsurface_weight") {
      ParseFloats(value, &out->subsurface, 1);
    } else if (name == "subsurface_color") {
      ParseFloats(value, out->subsurface_color, 3);
    } else if (name == "fuzz_weight") {
      ParseFloats(value, &fuzz_weight, 1);
    } else if (name == "fuzz_color") {
      ParseFloats(value, fuzz_color, 3);
    } else if (name == "fuzz_roughness") {
      ParseFloats(value, &out->sheen_roughness, 1);
    } else if (name == "coat_weight") {
      ParseFloats(value, &out->clearcoat, 1);
    } else if (name == "coat_color") {
      ParseFloats(value, out->coat_color, 3);
    } else if (name == "coat_roughness") {
      ParseFloats(value, &out->clearcoat_roughness, 1);
    } else if (name == "coat_ior") {
      ParseFloats(value, &out->coat_ior, 1);
    } else if (name == "coat_darkening") {
      ParseFloats(value, &out->coat_darkening, 1);
    } else if (name == "thin_film_weight") {
      ParseFloats(value, &out->iridescence, 1);
    } else if (name == "thin_film_thickness") {
      ParseFloats(value, &thin_film_thickness_um, 1);
    } else if (name == "thin_film_ior") {
      ParseFloats(value, &out->thin_film_ior, 1);
    } else if (name == "emission_luminance") {
      ParseFloats(value, &emission_luminance, 1);
    } else if (name == "emission_color") {
      ParseFloats(value, emission_color, 3);
    } else if (name == "geometry_opacity") {
      ParseFloats(value, &opacity, 1);
    }
  });

  // base_weight scales base_color, which is both the diffuse albedo and the
  // metal F0. Fuzz folds onto the engine's Charlie sheen lobe (the spec asks
  // for a Zeltner microflake LTC, which the engine does not implement).
  // emission_luminance is an absolute luminance in nits and emission_color a
  // possibly-HDR multiplier; that product is passed through literally and left
  // to the auto exposure rather than rescaled by an invented constant.
  for (int i = 0; i < 3; ++i) {
    out->base_color_factor[i] *= base_weight;
    out->sheen_color[i] = fuzz_color[i] * fuzz_weight;
    out->emissive_factor[i] = emission_color[i] * emission_luminance;
  }
  out->base_color_factor[3] = opacity;
  out->anisotropy = OpenPbrAnisotropyToEngine(anisotropy);
  // Micrometres in the document, nanometres in the engine.
  out->iridescence_thickness = thin_film_thickness_um * 1000.0f;
}

}  // namespace

bool LoadMaterialX(const std::string& path, Material* out) {
  std::ifstream file(path);
  if (!file) {
    RX_WARN("materialx: cannot open {}", path);
    return false;
  }
  std::stringstream buf;
  buf << file.rdbuf();
  std::string doc = buf.str();

  // A document may define both (an OpenPBR material with a standard_surface
  // fallback is a common authoring pattern); OpenPBR is the richer model.
  const bool open_pbr = doc.find("open_pbr_surface") != std::string::npos;
  if (open_pbr) {
    ApplyOpenPbrDefaults(out);
    ParseOpenPbrSurface(doc, out);
  } else if (doc.find("standard_surface") != std::string::npos) {
    ParseStandardSurface(doc, out);
  } else {
    RX_WARN("materialx: {} has no open_pbr_surface or standard_surface node", path);
    return false;
  }

  if (out->transmission > 0.0f || out->base_color_factor[3] < 1.0f)
    out->alpha_mode = AlphaMode::kBlend;
  RX_INFO("materialx: loaded {} from {}",
          open_pbr ? "open_pbr_surface" : "standard_surface", path);
  return true;
}

}  // namespace rx::asset
