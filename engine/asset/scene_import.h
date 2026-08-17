#ifndef RX_ASSET_SCENE_IMPORT_H_
#define RX_ASSET_SCENE_IMPORT_H_

#include <string>

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

  // An authored light. Scene files carry their own lighting rig, and a scene
  // lit by the viewer's procedural sky instead of the rig it shipped with does
  // not resemble the source at all. Positions/directions are in engine space
  // (y-up, metres); intensity is the source's own photometric value, which the
  // consumer scales (see kUsdDistantToEngine and friends in usd_loader.cc).
  struct Light {
    enum class Kind : u8 {
      kDistant, // directional; drives the engine sun
      kDome,    // environment/ibl
      kSphere,  // includes point (radius ~ 0)
      kRect,
      kDisk,
      kCylinder,
    };
    Kind kind = Kind::kSphere;
    Vec3 position{};
    Vec3 direction{0, -1, 0};
    f32 color[3] = {1, 1, 1};
    f32 intensity = 1.0f;
    f32 exposure = 0.0f;     // stops, applied on top of intensity
    f32 radius = 0.0f;       // sphere/disk, metres
    f32 width = 0.0f;        // rect, metres
    f32 height = 0.0f;       // rect, metres
    f32 length = 0.0f;       // cylinder, metres
    f32 cone_angle = 180.0f; // ShapingAPI cone, degrees (180 = no cone)
    f32 cone_softness = 0.0f;
    bool normalize = false; // divide by area (UsdLux `normalize`)
    std::string texture;    // dome/rect environment map, resolved to disk
    // Chromaticity of `texture`, solid-angle averaged and normalized to unit
    // luminance, or (1,1,1) with no texture. A dome's `color` is only a tint on
    // top of its environment map, so the map is what decides what colour the
    // sky fill actually is - and a warm key balanced against a fill taken from
    // the tint alone instead of the map lands nowhere near the source.
    f32 texture_average[3] = {1, 1, 1};
  };
  base::Vector<Light> lights;

  // An authored camera. The framing a scene ships with is usually the one it
  // was built to be seen from.
  struct Camera {
    Vec3 position{};
    f32 rotation[4] = {0, 0, 0, 1}; // quaternion x y z w
    f32 yfov = 0.9f;                // radians
    f32 znear = 0.1f;
    f32 zfar = 10000.0f;
  };
  base::Vector<Camera> cameras;
};

} // namespace rx::asset

#endif // RX_ASSET_SCENE_IMPORT_H_
