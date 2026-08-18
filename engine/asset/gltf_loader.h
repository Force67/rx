#ifndef RX_ASSET_GLTF_LOADER_H_
#define RX_ASSET_GLTF_LOADER_H_

#include <string>

#include "asset/scene_import.h"
#include "core/export.h"

namespace rx::asset {

// Loads .gltf or .glb into an ImportedScene including external buffers and
// images. Skinned mesh-node transforms are ignored as required by glTF.
// Generates tangents from uv derivatives when the source has none. Returns
// false and logs on malformed input.
RX_ASSET_EXPORT bool LoadGltfScene(const std::string &path, ImportedScene *out);

} // namespace rx::asset

#endif // RX_ASSET_GLTF_LOADER_H_
