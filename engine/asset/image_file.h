#ifndef RX_ASSET_IMAGE_FILE_H_
#define RX_ASSET_IMAGE_FILE_H_

#include <string>

#include "asset/asset_id.h"
#include "asset/texture.h"
#include "core/export.h"

namespace rx::asset {

// Images read straight off the real filesystem by path, rather than out of the
// Vfs through an AssetDatabase texture converter.
//
// Deliberately not a converter, for the reason Model.path imports its glTF
// itself instead of going through LoadMesh: content a scene merely POINTS AT
// (a .rxscene texture map, a MaterialX document's file inputs) names a path
// relative to the working directory, and nothing has mounted that directory.
// Whatever stb_image decodes is accepted, which covers png, jpg, tga and bmp -
// the formats a CC0 texture set actually ships in.

// Why `path` is not a decodable image, or empty when it is. Reads only the
// header, so a --validate sweep over a scene full of 4K maps costs no decode.
// LoadImageFile goes through the same reader, so a path this accepts is one
// that loads.
RX_ASSET_EXPORT std::string ImageFileProblem(const std::string& path);

// Decodes to an opaque rgba8 Texture with a single mip (the material system
// generates the chain at upload). `srgb` tags the result for the colour slots
// (base colour, emissive); data maps - normal, roughness, metallic, occlusion -
// must pass false, or the gpu linearizes values that were never encoded and the
// surface comes back visibly too smooth and too flat.
RX_ASSET_EXPORT bool LoadImageFile(const std::string& path, bool srgb, AssetId id, Texture* out);

}  // namespace rx::asset

#endif  // RX_ASSET_IMAGE_FILE_H_
