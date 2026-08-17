#ifndef RX_ASSET_USD_LOADER_H_
#define RX_ASSET_USD_LOADER_H_

#include <string>
#include <string_view>

#include "asset/scene_import.h"
#include "core/export.h"

namespace rx::asset {

// True for the four OpenUSD file extensions: .usd (either encoding), .usda
// (ascii), .usdc (crate binary), .usdz (zip package).
RX_ASSET_EXPORT bool IsUsdPath(std::string_view path);

// Loads a USD stage into an ImportedScene. Composition (sublayers, references,
// payloads, variants, class inherits) is resolved first, then the composed
// stage is flattened the same way LoadGltfScene flattens a glTF: node
// transforms baked to world space, polygons triangulated, GeomSubsets bound to
// materialBind turned into submeshes, UsdPreviewSurface mapped onto the engine
// metallic-roughness material, textures decoded to rgba8.
//
// The stage's own units are normalized away: `upAxis = "Z"` is rotated into the
// engine's y-up, and `metersPerUnit` is folded into the instance transforms.
//
// Returns false and logs on a stage that fails to open. A stage that opens but
// carries geometry the importer cannot represent still returns true, with the
// skipped prims logged.
RX_ASSET_EXPORT bool LoadUsdScene(const std::string &path, ImportedScene *out);

} // namespace rx::asset

#endif // RX_ASSET_USD_LOADER_H_
