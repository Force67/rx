// KHR_texture_transform, which Blender's exporter emits for any Mapping node
// that scales uvs. The importer bakes it into the vertex uvs, so what is under
// test is the arithmetic (offset/rotation/scale, in the extension's own order)
// and the one case a per-vertex bake cannot express: two maps of one material
// asking for different transforms, where the first wins and the loader warns
// rather than picking in silence.
//
// Three quads sharing one set of accessors, so the only difference between them
// is the material each names.

#include <cmath>
#include <cstdio>
#include <filesystem>

#include "asset/gltf_loader.h"

namespace {

using namespace rx;

// uv runs 0 0 -> 1 0 -> 1 1 -> 0 1 over one quad, which makes every expected
// value below readable as "the corner, transformed".
constexpr char kDocument[] = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1, 2]}],
  "nodes": [
    {"mesh": 0}, {"mesh": 1}, {"mesh": 2}
  ],
  "meshes": [
    {"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                     "indices": 2, "material": 0}]},
    {"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                     "indices": 2, "material": 1}]},
    {"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                     "indices": 2, "material": 2}]}
  ],
  "materials": [
    {"name": "transformed", "pbrMetallicRoughness": {"baseColorTexture": {
      "index": 0, "extensions": {"KHR_texture_transform": {
        "offset": [0.25, 0.5], "scale": [4, 2]}}}}},
    {"name": "plain", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}},
    {"name": "conflicting", "pbrMetallicRoughness": {"baseColorTexture": {
        "index": 0, "extensions": {"KHR_texture_transform": {"scale": [4, 4]}}}},
     "normalTexture": {"index": 0, "extensions": {
        "KHR_texture_transform": {"scale": [2, 2]}}}}
  ],
  "textures": [{"source": 0}],
  "images": [{"uri": "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3",
     "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2"},
    {"bufferView": 2, "componentType": 5123, "count": 6, "type": "SCALAR"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 48},
    {"buffer": 0, "byteOffset": 48, "byteLength": 32},
    {"buffer": 0, "byteOffset": 80, "byteLength": 12}
  ],
  "buffers": [{"byteLength": 92,
    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAACAPwAAgD8AAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAgD8AAIA/AAAAAAAAgD8AAAEAAgAAAAIAAwA="}]
})";

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "gltf_texture_transform_test: FAIL: %s\n", message);
  ++failures;
}

bool UvIs(const asset::Mesh &mesh, u32 vertex, f32 u, f32 v) {
  if (mesh.lods.empty() || mesh.lods[0].vertices.size() <= vertex)
    return false;
  const asset::Vertex &out = mesh.lods[0].vertices[vertex];
  return std::fabs(out.uv[0] - u) < 1e-5f && std::fabs(out.uv[1] - v) < 1e-5f;
}

} // namespace

int main() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rx_texture_transform.gltf";
  std::FILE *file = std::fopen(path.string().c_str(), "wb");
  if (!file) {
    std::fprintf(stderr, "gltf_texture_transform_test: cannot create fixture\n");
    return 1;
  }
  std::fwrite(kDocument, 1, sizeof(kDocument) - 1, file);
  std::fclose(file);

  asset::ImportedScene scene;
  const bool loaded = asset::LoadGltfScene(path.string(), &scene);
  std::filesystem::remove(path);
  Check(loaded, "generated glTF loads");
  if (!loaded)
    return 1;
  Check(scene.meshes.size() == 3, "one mesh per quad");
  if (scene.meshes.size() != 3)
    return 1;

  // scale then offset, per the extension's T * R * S: the far corner lands at
  // 1 * 4 + 0.25 across and 1 * 2 + 0.5 up. Without the bake it would still be
  // 1 1, and the texture would tile once where the author asked for four.
  Check(UvIs(scene.meshes[0], 0, 0.25f, 0.5f) &&
            UvIs(scene.meshes[0], 1, 4.25f, 0.5f) &&
            UvIs(scene.meshes[0], 2, 4.25f, 2.5f) &&
            UvIs(scene.meshes[0], 3, 0.25f, 2.5f),
        "offset and scale are baked into the uvs");

  // The other half of the rule: a material without the extension is untouched,
  // so nothing that used to import correctly moves.
  Check(UvIs(scene.meshes[1], 0, 0.0f, 0.0f) &&
            UvIs(scene.meshes[1], 2, 1.0f, 1.0f),
        "a material without the extension keeps its authored uvs");

  // One uv per vertex cannot serve two densities. The first slot read wins,
  // which is the base colour, and the loader warns naming the dropped one.
  Check(UvIs(scene.meshes[2], 2, 4.0f, 4.0f),
        "conflicting slots resolve to the base colour's transform");

  if (failures == 0) {
    std::puts("gltf_texture_transform_test: PASS");
    return 0;
  }
  return 1;
}
