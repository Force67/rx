// OpenPBR Surface import: the open_pbr_surface mapping, the unit and
// parametrization conversions it has to make on the way in, the spec defaults
// that apply to unauthored inputs, and the guarantee that a legacy
// standard_surface document is not silently reinterpreted as OpenPBR.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#include "asset/materialx.h"

namespace {

using namespace rx;

// Every input the engine maps, authored to a distinct value so a mis-wired
// field shows up as a specific failure rather than a plausible-looking one.
constexpr char kOpenPbrDoc[] = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="SR_full" type="surfaceshader">
    <input name="base_weight" type="float" value="0.5" />
    <input name="base_color" type="color3" value="0.4, 0.6, 0.8" />
    <input name="base_diffuse_roughness" type="float" value="0.75" />
    <input name="base_metalness" type="float" value="1.0" />
    <input name="specular_weight" type="float" value="0.9" />
    <input name="specular_color" type="color3" value="0.9, 0.7, 0.5" />
    <input name="specular_roughness" type="float" value="0.2" />
    <input name="specular_ior" type="float" value="1.8" />
    <input name="specular_roughness_anisotropy" type="float" value="0.5" />
    <input name="transmission_weight" type="float" value="0.6" />
    <input name="subsurface_weight" type="float" value="0.3" />
    <input name="subsurface_color" type="color3" value="0.7, 0.2, 0.1" />
    <input name="fuzz_weight" type="float" value="0.5" />
    <input name="fuzz_color" type="color3" value="1.0, 0.5, 0.25" />
    <input name="fuzz_roughness" type="float" value="0.6" />
    <input name="coat_weight" type="float" value="0.8" />
    <input name="coat_color" type="color3" value="0.2, 0.4, 0.6" />
    <input name="coat_roughness" type="float" value="0.05" />
    <input name="coat_ior" type="float" value="1.7" />
    <input name="coat_darkening" type="float" value="0.25" />
    <input name="thin_film_weight" type="float" value="1.0" />
    <input name="thin_film_thickness" type="float" value="0.35" />
    <input name="thin_film_ior" type="float" value="1.45" />
    <input name="emission_luminance" type="float" value="4.0" />
    <input name="emission_color" type="color3" value="1.0, 0.5, 0.25" />
    <input name="geometry_opacity" type="float" value="0.75" />
  </open_pbr_surface>
</materialx>
)";

// Authors nothing, so every value has to come from the OpenPBR defaults rather
// than the engine's glTF-derived ones.
constexpr char kOpenPbrBareDoc[] = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <open_pbr_surface name="SR_bare" type="surfaceshader">
    <input name="base_metalness" type="float" value="0.5" />
  </open_pbr_surface>
</materialx>
)";

// Autodesk Standard Surface: a different vocabulary that happens to share some
// input names. It must keep taking the legacy path.
constexpr char kStandardSurfaceDoc[] = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <standard_surface name="SR_legacy" type="surfaceshader">
    <input name="base" type="float" value="0.9" />
    <input name="base_color" type="color3" value="1.0, 0.5, 0.0" />
    <input name="metalness" type="float" value="0.25" />
    <input name="specular_roughness" type="float" value="0.4" />
    <input name="specular_IOR" type="float" value="1.45" />
    <input name="coat" type="float" value="0.3" />
    <input name="thin_film_thickness" type="float" value="250" />
  </standard_surface>
</materialx>
)";

constexpr char kUnknownDoc[] = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <disney_brdf name="SR_other" type="surfaceshader" />
</materialx>
)";

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "openpbr_test: FAIL: %s\n", message);
  ++failures;
}

bool Write(const std::filesystem::path &path, const char *text, size_t size) {
  std::FILE *file = std::fopen(path.string().c_str(), "wb");
  if (!file)
    return false;
  const bool ok = std::fwrite(text, 1, size, file) == size;
  std::fclose(file);
  return ok;
}

bool Near(f32 value, f32 expected) { return std::fabs(value - expected) < 1e-4f; }

bool Near3(const f32 *value, f32 x, f32 y, f32 z) {
  return Near(value[0], x) && Near(value[1], y) && Near(value[2], z);
}

// Writes `text` to a temp .mtlx, loads it, removes it. Returns load success.
bool LoadDoc(const char *name, const char *text, size_t size, asset::Material *out) {
  const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
  if (!Write(path, text, size)) {
    std::fprintf(stderr, "openpbr_test: cannot create fixture %s\n", name);
    return false;
  }
  const bool loaded = asset::LoadMaterialX(path.string(), out);
  std::filesystem::remove(path);
  return loaded;
}

void CheckOpenPbrFullMapping() {
  asset::Material m;
  if (!LoadDoc("rx_openpbr_full.mtlx", kOpenPbrDoc, sizeof(kOpenPbrDoc) - 1, &m)) {
    Check(false, "an open_pbr_surface document loads");
    return;
  }

  // base_weight is a multiplier on base_color, not a separate engine field.
  Check(Near3(m.base_color_factor, 0.2f, 0.3f, 0.4f),
        "base_weight scales base_color into the base color factor");
  Check(Near(m.base_color_factor[3], 0.75f),
        "geometry_opacity becomes the base color alpha");
  Check(Near(m.base_diffuse_roughness, 0.75f), "base_diffuse_roughness carries over");
  Check(Near(m.metallic_factor, 1.0f), "base_metalness becomes the metallic factor");

  Check(Near(m.roughness_factor, 0.2f), "specular_roughness becomes the roughness factor");
  Check(Near(m.ior, 1.8f), "specular_ior becomes the ior");
  Check(Near(m.specular_weight, 0.9f), "specular_weight carries over");
  Check(Near3(m.specular_color, 0.9f, 0.7f, 0.5f), "specular_color carries over");

  // OpenPBR anisotropy a in [0,1] maps to the engine's k in [-1,1] by matching
  // the NDF axis ratio: k = a/(2-a), so 0.5 -> 1/3.
  Check(Near(m.anisotropy, 1.0f / 3.0f),
        "specular_roughness_anisotropy is reparametrized to the engine's range");

  Check(Near(m.transmission, 0.6f), "transmission_weight carries over");
  Check(Near(m.subsurface, 0.3f), "subsurface_weight carries over");
  Check(Near3(m.subsurface_color, 0.7f, 0.2f, 0.1f), "subsurface_color carries over");

  // Fuzz folds onto the engine's sheen lobe, weight multiplied into the colour.
  Check(Near3(m.sheen_color, 0.5f, 0.25f, 0.125f),
        "fuzz_weight scales fuzz_color into the sheen colour");
  Check(Near(m.sheen_roughness, 0.6f), "fuzz_roughness becomes the sheen roughness");

  Check(Near(m.clearcoat, 0.8f), "coat_weight becomes the clearcoat weight");
  Check(Near(m.clearcoat_roughness, 0.05f), "coat_roughness carries over");
  Check(Near(m.coat_ior, 1.7f), "coat_ior carries over");
  Check(Near(m.coat_darkening, 0.25f), "coat_darkening carries over");
  Check(Near3(m.coat_color, 0.2f, 0.4f, 0.6f), "coat_color carries over");

  Check(Near(m.iridescence, 1.0f), "thin_film_weight becomes the iridescence weight");
  Check(Near(m.thin_film_ior, 1.45f), "thin_film_ior carries over");
  // The spec states thickness in micrometres; the engine works in nanometres.
  Check(Near(m.iridescence_thickness, 350.0f),
        "thin_film_thickness converts from micrometres to nanometres");

  // emission_luminance is an absolute luminance, emission_color a multiplier.
  Check(Near3(m.emissive_factor, 4.0f, 2.0f, 1.0f),
        "emission_luminance times emission_color becomes the emissive factor");

  Check(m.alpha_mode == asset::AlphaMode::kBlend,
        "a transmissive, partly transparent surface routes to the blend pass");
}

void CheckOpenPbrDefaults() {
  asset::Material m;
  if (!LoadDoc("rx_openpbr_bare.mtlx", kOpenPbrBareDoc, sizeof(kOpenPbrBareDoc) - 1,
               &m)) {
    Check(false, "a near-empty open_pbr_surface document loads");
    return;
  }

  Check(Near(m.metallic_factor, 0.5f), "the one authored input still applies");
  // These all differ from the engine's own (glTF) defaults, so seeing the
  // OpenPBR value is what proves the defaults were seeded.
  Check(Near3(m.base_color_factor, 0.8f, 0.8f, 0.8f),
        "unauthored base_color defaults to the OpenPBR 0.8 grey, not white");
  Check(Near(m.roughness_factor, 0.3f),
        "unauthored specular_roughness defaults to 0.3, not 1");
  Check(Near(m.coat_ior, 1.6f), "unauthored coat_ior defaults to 1.6, not 1.5");
  Check(Near(m.coat_darkening, 1.0f),
        "unauthored coat_darkening defaults to fully physical");
  Check(Near(m.thin_film_ior, 1.4f), "unauthored thin_film_ior defaults to 1.4, not 1.3");
  Check(Near(m.iridescence_thickness, 500.0f),
        "unauthored thin_film_thickness defaults to 0.5um");
  Check(Near(m.sheen_roughness, 0.5f),
        "unauthored fuzz_roughness defaults to 0.5, not the glTF sheen 0.3");
  Check(Near3(m.subsurface_color, 0.8f, 0.8f, 0.8f),
        "unauthored subsurface_color defaults to the OpenPBR 0.8 grey");
}

void CheckStandardSurfaceStillWorks() {
  asset::Material m;
  if (!LoadDoc("rx_standard_surface.mtlx", kStandardSurfaceDoc,
               sizeof(kStandardSurfaceDoc) - 1, &m)) {
    Check(false, "a standard_surface document still loads");
    return;
  }

  Check(Near3(m.base_color_factor, 0.9f, 0.45f, 0.0f),
        "standard_surface base scales base_color");
  Check(Near(m.metallic_factor, 0.25f), "standard_surface metalness maps to metallic");
  Check(Near(m.roughness_factor, 0.4f), "standard_surface specular_roughness maps over");
  Check(Near(m.ior, 1.45f), "standard_surface specular_IOR maps over");
  Check(Near(m.clearcoat, 0.3f), "standard_surface coat maps to clearcoat");
  // standard_surface states thin film thickness in nanometres already, so the
  // OpenPBR micrometre conversion must not be applied to it.
  Check(Near(m.iridescence_thickness, 250.0f),
        "standard_surface thin film thickness stays in nanometres");
  // The legacy path must not be handed OpenPBR's defaults.
  Check(Near(m.coat_ior, 1.5f), "standard_surface keeps the engine coat ior default");
  Check(Near(m.coat_darkening, 0.0f),
        "standard_surface keeps coat darkening off, preserving its historical look");
}

void CheckUnknownShaderRejected() {
  asset::Material m;
  const bool loaded =
      LoadDoc("rx_unknown.mtlx", kUnknownDoc, sizeof(kUnknownDoc) - 1, &m);
  Check(!loaded, "a document with neither surface node is rejected");
}

bool Finite(f32 v) { return std::isfinite(v); }

bool Finite3(const f32 *v) { return Finite(v[0]) && Finite(v[1]) && Finite(v[2]); }

// Opt-in sweep over the upstream example corpus (tools/get_openpbr_samples.sh),
// which is gitignored, so this stays quiet when the assets are absent. Catches
// the failure mode the hand-written fixtures cannot: a real authored document
// that parses into something the renderer would choke on.
void CheckExampleCorpus() {
  const char *dir_env = std::getenv("RX_OPENPBR_EXAMPLES");
  if (dir_env == nullptr)
    return;
  const std::filesystem::path dir(dir_env);
  if (!std::filesystem::is_directory(dir)) {
    Check(false, "RX_OPENPBR_EXAMPLES points at a directory");
    return;
  }

  int loaded = 0, rejected = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir)) {
    if (entry.path().extension() != ".mtlx")
      continue;
    asset::Material m;
    if (!asset::LoadMaterialX(entry.path().string(), &m)) {
      std::fprintf(stderr, "openpbr_test: rejected %s\n",
                   entry.path().filename().string().c_str());
      ++rejected;
      continue;
    }
    ++loaded;

    const std::string name = entry.path().filename().string();
    const auto fail = [&](const char *what) {
      std::fprintf(stderr, "openpbr_test: FAIL: %s: %s\n", name.c_str(), what);
      ++failures;
    };
    if (!Finite3(m.base_color_factor) || !Finite(m.base_color_factor[3]))
      fail("base color factor is finite");
    if (!Finite3(m.specular_color) || !Finite3(m.coat_color) ||
        !Finite3(m.sheen_color) || !Finite3(m.emissive_factor) ||
        !Finite3(m.subsurface_color))
      fail("colour inputs are finite");
    if (!Finite(m.roughness_factor) || !Finite(m.metallic_factor) ||
        !Finite(m.ior) || !Finite(m.coat_ior) || !Finite(m.thin_film_ior) ||
        !Finite(m.iridescence_thickness) || !Finite(m.anisotropy))
      fail("scalar inputs are finite");
    // Ranges the shader relies on. Roughness and metallic are clamped there
    // too, but an out-of-range value here means the import is wrong.
    if (m.roughness_factor < 0.0f || m.roughness_factor > 1.0f)
      fail("specular_roughness lands in [0,1]");
    if (m.metallic_factor < 0.0f || m.metallic_factor > 1.0f)
      fail("base_metalness lands in [0,1]");
    if (m.anisotropy < -1.0f || m.anisotropy > 1.0f)
      fail("anisotropy lands in [-1,1]");
    // An ior at or below 1 makes the dielectric f0 degenerate.
    if (m.ior < 1.0f || m.coat_ior < 1.0f || m.thin_film_ior < 1.0f)
      fail("refractive indices are at least 1");
    if (m.clearcoat < 0.0f || m.clearcoat > 1.0f)
      fail("coat_weight lands in [0,1]");
  }

  Check(rejected == 0, "every example material parses");
  Check(loaded > 50, "the example corpus is actually present and populated");
  std::printf("openpbr_test: swept %d example material(s)\n", loaded);

  // Two spot checks against values read straight out of the source documents,
  // so the sweep cannot pass by importing everything as defaults.
  asset::Material brass;
  if (asset::LoadMaterialX((dir / "open_pbr_brass.mtlx").string(), &brass)) {
    Check(Near3(brass.base_color_factor, 0.844f, 0.782f, 0.473f) &&
              Near(brass.metallic_factor, 1.0f) &&
              Near(brass.roughness_factor, 0.02f),
          "brass imports its authored metal values");
    // Deliberately above 1 in the source: an edge tint brighter than the
    // normal-incidence colour. It must survive rather than be clamped.
    Check(Near3(brass.specular_color, 0.963f, 0.977f, 1.013f),
          "brass keeps its greater-than-one specular edge tint");
  }
  asset::Material carpaint;
  if (asset::LoadMaterialX((dir / "open_pbr_carpaint.mtlx").string(), &carpaint)) {
    Check(Near(carpaint.clearcoat, 1.0f) && Near(carpaint.clearcoat_roughness, 0.02f) &&
              Near(carpaint.coat_ior, 1.6f),
          "car paint imports its coat");
    Check(Near(carpaint.coat_darkening, 1.0f),
          "car paint gets the OpenPBR coat darkening default it never authors");
  }
}

} // namespace

int main() {
  CheckOpenPbrFullMapping();
  CheckOpenPbrDefaults();
  CheckStandardSurfaceStillWorks();
  CheckUnknownShaderRejected();
  CheckExampleCorpus();

  if (failures == 0) {
    std::puts("openpbr_test: PASS");
    return 0;
  }
  std::fprintf(stderr, "openpbr_test: %d failure(s)\n", failures);
  return 1;
}
