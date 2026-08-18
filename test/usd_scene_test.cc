#include <cmath>
#include <cstdio>
#include <filesystem>

#include "asset/usd_loader.h"

namespace {

using namespace rx;

// The prop lives in its own layer so the main stage has to resolve a reference
// arc to see any geometry at all: composition, not just parsing, is what makes
// this scene load.
constexpr char kPropLayer[] = R"(#usda 1.0

def Xform "Prop"
{
    def Mesh "Quad"
    {
        int[] faceVertexCounts = [3, 3]
        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3]
        point3f[] points = [(0, 0, 0), (2, 0, 0), (2, 2, 0), (0, 2, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </Prop/Paint>
    }

    def Material "Paint"
    {
        token outputs:surface.connect = </Prop/Paint/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.25, 0.5, 0.75)
            float inputs:metallic = 0.125
            float inputs:roughness = 0.375
            token outputs:surface
        }
    }
}
)";

// Z-up and centimetres, the Omniverse authoring defaults: the importer has to
// normalize both away.
constexpr char kStage[] = R"(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 0.01
    upAxis = "Z"
)

def Xform "World"
{
    def "Placed" (
        prepend references = @rx_usd_prop.usda@</Prop>
    )
    {
        double3 xformOp:translate = (0, 200, 300)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
}
)";

// A MaterialX OpenPBR surface, the other shader tydra can hand back. Single
// layer: composition is already covered above, this is about the material.
constexpr char kOpenPbrStage[] = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "Quad"
    {
        int[] faceVertexCounts = [3]
        int[] faceVertexIndices = [0, 1, 2]
        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]
        normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1)] (
            interpolation = "vertex"
        )
        texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1)] (
            interpolation = "vertex"
        )
        rel material:binding = </World/Coated>
    }

    def Material "Coated"
    {
        token outputs:surface.connect = </World/Coated/Surface.outputs:surface>

        def Shader "Surface"
        {
            uniform token info:id = "ND_open_pbr_surface_surfaceshader"
            float inputs:base_weight = 0.5
            color3f inputs:base_color = (0.4, 0.6, 0.8)
            float inputs:base_metalness = 1
            float inputs:base_diffuse_roughness = 0.75
            float inputs:specular_roughness = 0.2
            float inputs:specular_ior = 1.8
            float inputs:specular_roughness_anisotropy = 0.5
            color3f inputs:specular_color = (0.9, 0.7, 0.5)
            float inputs:coat_weight = 0.8
            float inputs:coat_ior = 1.7
            color3f inputs:coat_color = (0.2, 0.4, 0.6)
            float inputs:thin_film_weight = 1
            float inputs:thin_film_thickness = 0.35
            token outputs:surface
        }
    }
}
)";

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "usd_scene_test: FAIL: %s\n", message);
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

} // namespace

int main() {
  Check(asset::IsUsdPath("scene.usd") && asset::IsUsdPath("scene.USDA") &&
            asset::IsUsdPath("a/b.usdc") && asset::IsUsdPath("packed.usdz"),
        "every usd extension is recognized");
  Check(!asset::IsUsdPath("scene.gltf") && !asset::IsUsdPath("usd") &&
            !asset::IsUsdPath("no_extension"),
        "non-usd paths are not claimed");

  const std::filesystem::path dir = std::filesystem::temp_directory_path();
  const std::filesystem::path prop = dir / "rx_usd_prop.usda";
  const std::filesystem::path stage = dir / "rx_usd_stage.usda";
  if (!Write(prop, kPropLayer, sizeof(kPropLayer) - 1) ||
      !Write(stage, kStage, sizeof(kStage) - 1)) {
    std::fprintf(stderr, "usd_scene_test: cannot create fixture\n");
    return 1;
  }

  asset::ImportedScene scene;
  const bool loaded = asset::LoadUsdScene(stage.string(), &scene);
  std::filesystem::remove(prop);
  std::filesystem::remove(stage);
  Check(loaded, "referenced usda stage loads");
  if (!loaded) {
    std::fprintf(stderr, "usd_scene_test: FAIL\n");
    return 1;
  }

  Check(scene.meshes.size() == 1, "the referenced mesh is composed in");
  Check(scene.materials.size() == 1, "the referenced material is composed in");
  Check(scene.instances.size() == 1, "the referencing xform places one instance");

  if (scene.meshes.size() == 1 && !scene.meshes[0].lods.empty()) {
    const asset::MeshLod &lod = scene.meshes[0].lods[0];
    Check(lod.indices.size() == 6, "both triangles survive");
    Check(lod.vertices.size() >= 3 && lod.vertices.size() <= 6,
          "the quad's corners stay single-indexable");
    Check(lod.submeshes.size() == 1, "one material binding is one submesh");
    if (lod.submeshes.size() == 1 && scene.materials.size() == 1) {
      Check(lod.submeshes[0].material == scene.materials[0].id,
            "the submesh points at the bound material");
    }
  } else {
    Check(false, "the mesh converted to an engine lod");
  }

  if (scene.materials.size() == 1) {
    const asset::Material &material = scene.materials[0];
    Check(Near(material.base_color_factor[0], 0.25f) &&
              Near(material.base_color_factor[1], 0.5f) &&
              Near(material.base_color_factor[2], 0.75f),
          "UsdPreviewSurface diffuseColor becomes the base color factor");
    Check(Near(material.metallic_factor, 0.125f) &&
              Near(material.roughness_factor, 0.375f),
          "metallic and roughness carry over");
  }

  if (scene.instances.size() == 1) {
    const asset::ImportedScene::Instance &instance = scene.instances[0];
    // (0, 200, 300) stage units -> (0, 2, 3) metres z-up -> (0, 3, -2) y-up.
    Check(Near(instance.position.x, 0.0f) && Near(instance.position.y, 3.0f) &&
              Near(instance.position.z, -2.0f),
          "z-up centimetres are normalized to y-up metres");
    Check(Near(instance.scale, 0.01f),
          "metersPerUnit is folded into the instance scale");
  }

  // OpenPBR: tydra parses it into a separate slot on RenderMaterial, so the
  // importer has to reach for it instead of falling back to engine defaults.
  const std::filesystem::path openpbr_stage = dir / "rx_usd_openpbr.usda";
  if (!Write(openpbr_stage, kOpenPbrStage, sizeof(kOpenPbrStage) - 1)) {
    std::fprintf(stderr, "usd_scene_test: cannot create openpbr fixture\n");
    return 1;
  }
  asset::ImportedScene openpbr_scene;
  const bool openpbr_loaded =
      asset::LoadUsdScene(openpbr_stage.string(), &openpbr_scene);
  std::filesystem::remove(openpbr_stage);
  Check(openpbr_loaded, "an OpenPBR stage loads");

  if (openpbr_loaded && openpbr_scene.materials.size() == 1) {
    const asset::Material &m = openpbr_scene.materials[0];
    // Anything other than the engine defaults here proves the OpenPBR shader
    // was read rather than skipped: base_color would be white, not 0.4 * 0.5.
    Check(Near(m.base_color_factor[0], 0.2f) && Near(m.base_color_factor[1], 0.3f) &&
              Near(m.base_color_factor[2], 0.4f),
          "base_weight scales base_color into the base color factor");
    Check(Near(m.metallic_factor, 1.0f) && Near(m.roughness_factor, 0.2f),
          "base_metalness and specular_roughness carry over");
    Check(Near(m.base_diffuse_roughness, 0.75f), "base_diffuse_roughness carries over");
    Check(Near(m.ior, 1.8f), "specular_ior carries over");
    Check(Near(m.specular_color[0], 0.9f) && Near(m.specular_color[1], 0.7f) &&
              Near(m.specular_color[2], 0.5f),
          "specular_color carries over");
    Check(Near(m.clearcoat, 0.8f) && Near(m.coat_ior, 1.7f),
          "coat_weight and coat_ior carry over");
    Check(Near(m.coat_color[0], 0.2f) && Near(m.coat_color[1], 0.4f) &&
              Near(m.coat_color[2], 0.6f),
          "coat_color carries over");
    // k = a/(2-a) matches the NDF axis ratio; 0.5 -> 1/3.
    Check(Near(m.anisotropy, 1.0f / 3.0f),
          "specular_roughness_anisotropy is reparametrized to the engine's range");
    // tinyusdz passes the document value through untouched, and the spec states
    // it in micrometres, so the importer owns the conversion to nanometres.
    Check(Near(m.iridescence, 1.0f) && Near(m.iridescence_thickness, 350.0f),
          "thin film thickness converts from micrometres to nanometres");
  } else if (openpbr_loaded) {
    Check(false, "the OpenPBR stage yields exactly one material");
  }

  if (failures == 0) {
    std::puts("usd_scene_test: PASS");
    return 0;
  }
  return 1;
}
