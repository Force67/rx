#ifndef RX_ASSET_MATERIALX_H_
#define RX_ASSET_MATERIALX_H_

#include <string>

#include "asset/material.h"
#include "core/export.h"

namespace rx::asset {

// Loads the constant inputs of a MaterialX (.mtlx) surface shader into a
// Material. Both vocabularies are understood: OpenPBR Surface
// (`open_pbr_surface`, base_weight / base_metalness / coat_weight / fuzz_weight
// / thin_film_*) and Autodesk Standard Surface (`standard_surface`, base /
// metalness / coat / sheen), with OpenPBR preferred when a document defines
// both. For an OpenPBR document the OpenPBR defaults are seeded first, so
// unauthored inputs land on the spec value rather than the engine's
// glTF-derived one. Node-graph connected inputs (no constant value) are
// skipped. Returns false on a read error or when neither node is present.
RX_ASSET_EXPORT bool LoadMaterialX(const std::string& path, Material* out);

}  // namespace rx::asset

#endif  // RX_ASSET_MATERIALX_H_
