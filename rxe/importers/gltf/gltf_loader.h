#ifndef RX_IMPORTERS_GLTF_GLTF_LOADER_H_
#define RX_IMPORTERS_GLTF_GLTF_LOADER_H_


#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "rxe/asset/scene_import.h"

namespace rx::importers {

// Loads .gltf or .glb into an ImportedScene including external buffers and
// images. Skinned mesh-node transforms are ignored as required by glTF.
// Generates tangents from uv derivatives when the source has none. Returns
// false and logs on malformed input.
RX_GLTF_EXPORT bool LoadGltfScene(const base::String &path, asset::ImportedScene *out);

} // namespace rx::importers

#endif // RX_IMPORTERS_GLTF_GLTF_LOADER_H_
