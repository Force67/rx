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

  if (failures == 0) {
    std::puts("usd_scene_test: PASS");
    return 0;
  }
  return 1;
}
