#ifndef RX_ASSET_SCENE_IMPORT_H_
#define RX_ASSET_SCENE_IMPORT_H_

#include <base/containers/vector.h>

#include "asset/material.h"
#include "asset/mesh.h"
#include "asset/skeleton.h"
#include "asset/texture.h"
#include "core/math.h"

namespace rx::asset {

// What every scene-file importer produces: a flattened scene with static node
// transforms baked to world space, one engine Mesh per source mesh (source
// primitives become submeshes), textures decoded to rgba8. Asset ids derive
// from "<path>#<kind><index>" so scenes from different files never collide.
// Filled by LoadGltfScene (gltf_loader.h) and LoadUsdScene (usd_loader.h).
struct ImportedScene {
  base::Vector<Texture> textures;
  base::Vector<Material> materials;
  base::Vector<Mesh> meshes;
  // One runtime skeleton and exact palette binding per source skin. Skeleton
  // bones are topologically ordered; skin_bindings retain the source palette
  // order used by the joint indices and inverse bind matrices. A mesh may be
  // instanced with more than one of these bindings.
  base::Vector<Skeleton> skeletons;
  base::Vector<SkinBinding> skin_bindings;

  struct Instance {
    u32 mesh_index = 0;
    i32 skeleton_index = -1; // index into skeletons, -1 for a static mesh
    Vec3 position{};
    f32 rotation[4] = {0, 0, 0, 1}; // quaternion x y z w
    f32 scale = 1.0f;               // uniform; non uniform scale is averaged
  };
  base::Vector<Instance> instances;
};

} // namespace rx::asset

#endif // RX_ASSET_SCENE_IMPORT_H_
