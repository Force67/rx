#include <cmath>
#include <cstdio>
#include <filesystem>

#include "asset/gltf_loader.h"

namespace {

using namespace rx;

constexpr char kSharedMeshSkins[] = R"({
  "asset": {"version": "2.0"},
  "scene": 0,
  "scenes": [{"nodes": [0, 1, 2, 3]}],
  "nodes": [
    {"name": "joint_a"},
    {"name": "joint_b", "translation": [0, 1, 0]},
    {"name": "skin_a_instance", "mesh": 0, "skin": 0,
     "translation": [4, 5, 6], "scale": [2, 2, 2]},
    {"name": "skin_b_instance", "mesh": 0, "skin": 1,
     "translation": [-4, -5, -6], "scale": [3, 3, 3]}
  ],
  "skins": [
    {"joints": [0]},
    {"joints": [1]}
  ],
  "meshes": [{"primitives": [{"attributes": {
    "POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2
  }}]}],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
     "min": [0, 0, 0], "max": [1, 1, 0]},
    {"bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4"},
    {"bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4"}
  ],
  "bufferViews": [
    {"buffer": 0, "byteOffset": 0, "byteLength": 36},
    {"buffer": 0, "byteOffset": 36, "byteLength": 12},
    {"buffer": 0, "byteOffset": 48, "byteLength": 48}
  ],
  "buffers": [{"byteLength": 96,
    "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAAAA"}]
})";

int failures = 0;

void Check(bool condition, const char *message) {
  if (condition)
    return;
  std::fprintf(stderr, "gltf_skin_test: FAIL: %s\n", message);
  ++failures;
}

bool IdentityTransform(const asset::ImportedScene::Instance &instance) {
  return Length(instance.position) < 1e-7f &&
         std::fabs(instance.rotation[0]) < 1e-7f &&
         std::fabs(instance.rotation[1]) < 1e-7f &&
         std::fabs(instance.rotation[2]) < 1e-7f &&
         std::fabs(instance.rotation[3] - 1.0f) < 1e-7f &&
         std::fabs(instance.scale - 1.0f) < 1e-7f;
}

} // namespace

int main() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rx_shared_mesh_skins.gltf";
  std::FILE *file = std::fopen(path.string().c_str(), "wb");
  if (!file) {
    std::fprintf(stderr, "gltf_skin_test: cannot create fixture\n");
    return 1;
  }
  std::fwrite(kSharedMeshSkins, 1, sizeof(kSharedMeshSkins) - 1, file);
  std::fclose(file);

  asset::ImportedScene scene;
  const bool loaded = asset::LoadGltfScene(path.string(), &scene);
  std::filesystem::remove(path);
  Check(loaded, "generated glTF loads");
  if (loaded) {
    Check(scene.meshes.size() == 1 && scene.meshes[0].skinned,
          "shared source mesh keeps one geometry allocation");
    Check(scene.skeletons.size() == 2 && scene.skin_bindings.size() == 2,
          "every glTF skin has a skeleton and palette binding");
    Check(scene.skin_bindings[0].bones.size() == 1 &&
              scene.skin_bindings[0].bones[0] == "joint_a" &&
              scene.skin_bindings[1].bones.size() == 1 &&
              scene.skin_bindings[1].bones[0] == "joint_b",
          "bindings remain distinct when instances share a mesh");
    Check(scene.instances.size() == 2 &&
              scene.instances[0].skeleton_index == 0 &&
              scene.instances[1].skeleton_index == 1,
          "each instance retains its selected skin");
    Check(IdentityTransform(scene.instances[0]) &&
              IdentityTransform(scene.instances[1]),
          "skinned mesh-node transforms are ignored");
  }

  if (failures == 0) {
    std::puts("gltf_skin_test: PASS");
    return 0;
  }
  return 1;
}
