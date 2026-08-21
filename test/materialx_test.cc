// MaterialX documents as a texture library actually ships them: the surface
// shader's inputs are <image>/<tiledimage> nodes, not constants, so a loader
// that only read constants handed back a flat colour and dropped every map.
// What is under test is the resolution of those connections (direct, through a
// <normalmap>, and through a <nodegraph> output), that a connection this build
// cannot evaluate leaves the slot empty rather than guessing, and that the
// constant-input path a hand-written standard_surface uses still works.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "asset/materialx.h"

namespace {

using namespace rx;

namespace fs = std::filesystem;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "materialx_test: FAIL: %s\n", message);
  ++failures;
}

fs::path Write(const char* name, const std::string& text) {
  const fs::path path = fs::temp_directory_path() / name;
  std::ofstream out(path, std::ios::binary);
  out << text;
  return path;
}

// Filenames resolve against the document, so every expectation is the temp
// directory plus the name the document wrote.
std::string Beside(const char* file) {
  return (fs::temp_directory_path() / file).lexically_normal().string();
}

// What ambientCG ships: MaterialX 1.39, a flat list of nodes at the root, an
// open_pbr_surface, tiledimage nodes and a normalmap in front of the normal.
constexpr char kOpenPbrWithImages[] = R"(<?xml version="1.0"?>
<materialx version="1.39" fileprefix="./">
  <open_pbr_surface type="surfaceshader" name="Surf">
    <input type="color3" nodename="ColorImage" name="base_color" />
    <input type="vector3" nodename="NormalMap" name="geometry_normal" />
    <input type="float" nodename="RoughImage" name="specular_roughness" />
    <input type="float" value="0.25" name="base_metalness" />
  </open_pbr_surface>
  <tiledimage type="color3" name="ColorImage">
    <input type="filename" value="stone_color.png" name="file" colorspace="srgb_texture" />
  </tiledimage>
  <tiledimage type="vector3" name="NormalImage">
    <input type="filename" value="stone_normal.png" name="file" />
  </tiledimage>
  <normalmap type="vector3" name="NormalMap">
    <input type="vector3" nodename="NormalImage" name="in" />
  </normalmap>
  <tiledimage type="float" name="RoughImage">
    <input type="filename" value="stone_rough.png" name="file" />
  </tiledimage>
</materialx>
)";

// The other layout in the wild: a 1.38 standard_surface reaching into a
// nodegraph by output name, plus a connection through a node this build does
// not evaluate.
constexpr char kNodegraphStandardSurface[] = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <nodegraph name="NG">
    <image type="color3" name="albedo">
      <input type="filename" value="brick_color.png" name="file" />
    </image>
    <multiply type="float" name="scaled_rough">
      <input type="float" value="0.5" name="in2" />
    </multiply>
    <output type="color3" name="out_color" nodename="albedo" />
    <output type="float" name="out_rough" nodename="scaled_rough" />
  </nodegraph>
  <standard_surface type="surfaceshader" name="Surf">
    <input type="color3" nodegraph="NG" output="out_color" name="base_color" />
    <input type="float" nodegraph="NG" output="out_rough" name="specular_roughness" />
    <input type="float" value="1.7" name="specular_IOR" />
  </standard_surface>
</materialx>
)";

// No images at all, which is what a hand-authored preset looks like.
constexpr char kConstantsOnly[] = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <standard_surface type="surfaceshader" name="Surf">
    <input type="color3" value="0.2, 0.4, 0.6" name="base_color" />
    <input type="float" value="0.5" name="base" />
    <input type="float" value="1.0" name="metalness" />
    <input type="float" value="0.3" name="specular_roughness" />
    <input type="float" value="0.75" name="coat" />
  </standard_surface>
</materialx>
)";

void TestOpenPbrImages() {
  const fs::path path = Write("rx_openpbr.mtlx", kOpenPbrWithImages);
  asset::Material material;
  asset::MaterialXMaps maps;
  Check(asset::LoadMaterialX(path.string(), &material, &maps),
        "an open_pbr_surface document loads (a texture library ships nothing else)");
  Check(maps.base_color == Beside("stone_color.png"), "base colour resolves beside the document");
  // Through the normalmap node, which only converts the encoding the engine
  // already expects, so following it and taking the image is exact.
  Check(maps.normal == Beside("stone_normal.png"), "a normal reached through <normalmap> resolves");
  Check(maps.roughness == Beside("stone_rough.png"), "roughness resolves");
  Check(maps.metallic.empty(), "a constant input leaves its map slot empty");
  // OpenPBR spells it base_metalness; the engine field is the same one
  // standard_surface's metalness fills.
  Check(std::fabs(material.metallic_factor - 0.25f) < 1e-6f,
        "the OpenPBR spelling of a constant input maps onto the same field");
  fs::remove(path);
}

void TestNodegraphAndUnsupportedNode() {
  const fs::path path = Write("rx_nodegraph.mtlx", kNodegraphStandardSurface);
  asset::Material material;
  asset::MaterialXMaps maps;
  Check(asset::LoadMaterialX(path.string(), &material, &maps),
        "a nodegraph-style standard_surface document loads");
  Check(maps.base_color == Beside("brick_color.png"),
        "a nodegraph output resolves to the image behind it");
  // A <multiply> is a graph this build does not evaluate. Leaving the slot
  // empty (and warning by name) is the honest answer; binding the operand as if
  // it were the result would render a material nobody authored.
  Check(maps.roughness.empty(), "a connection through an unevaluated node binds nothing");
  Check(std::fabs(material.ior - 1.7f) < 1e-6f, "constants beside a connection still load");
  fs::remove(path);
}

void TestConstantsStillLoad() {
  const fs::path path = Write("rx_constants.mtlx", kConstantsOnly);
  asset::Material material;
  Check(asset::LoadMaterialX(path.string(), &material, nullptr),
        "a constants-only document loads with no maps requested");
  // base is a weight on base_color, not a field of its own.
  Check(std::fabs(material.base_color_factor[0] - 0.1f) < 1e-6f &&
            std::fabs(material.base_color_factor[2] - 0.3f) < 1e-6f,
        "base weight multiplies base_color");
  Check(std::fabs(material.metallic_factor - 1.0f) < 1e-6f &&
            std::fabs(material.roughness_factor - 0.3f) < 1e-6f &&
            std::fabs(material.clearcoat - 0.75f) < 1e-6f,
        "the standard_surface constants map onto the engine's lobes");
  fs::remove(path);
}

}  // namespace

int main() {
  TestOpenPbrImages();
  TestNodegraphAndUnsupportedNode();
  TestConstantsStillLoad();
  if (failures == 0) {
    std::puts("materialx_test: PASS");
    return 0;
  }
  return 1;
}
