#ifndef RX_SCENE_COMPONENTS_H_
#define RX_SCENE_COMPONENTS_H_

#include "asset/asset_id.h"
#include "core/types.h"

namespace rx::scene {

struct Transform {
  f32 position[3] = {0, 0, 0};
  f32 rotation[4] = {0, 0, 0, 1};  // quaternion
  f32 scale = 1.0f;
};

struct Renderable {
  asset::AssetId mesh;
};

// Tag marking an entity hidden; the render pass skips it. A tag carries no
// data, its presence is the state.
struct Hidden {};

}  // namespace rx::scene

#endif  // RX_SCENE_COMPONENTS_H_
