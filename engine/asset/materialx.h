#ifndef RX_ASSET_MATERIALX_H_
#define RX_ASSET_MATERIALX_H_

#include <string>

#include "asset/material.h"
#include "core/export.h"

namespace rx::asset {

// The image files a document's surface shader takes its maps from, already
// resolved against the DOCUMENT's own directory: a MaterialX <input
// type="filename"> is document-relative, unlike everything a .rxscene names,
// and a texture set is shipped as a folder that has to stay movable as a unit.
//
// Empty where the document gives that slot a constant, or nothing at all.
// LoadMaterialX only reports the files; opening them is the caller's, since
// publishing a texture needs a database and a gpu this module has neither of.
struct MaterialXMaps {
  std::string base_color;
  std::string normal;
  std::string roughness;
  std::string metallic;
  std::string occlusion;
  std::string emissive;
};

// Loads the first standard_surface or open_pbr_surface node of a MaterialX
// (.mtlx) document into a Material, mapping its inputs (base, metalness,
// specular_roughness, coat, sheen/fuzz, subsurface, transmission, thin film,
// emission, ...) onto the engine's pbr lobes under either vocabulary's spelling.
//
// Inputs CONNECTED to an <image>/<tiledimage> node resolve to a file in *maps
// (optionally through a <normalmap>), which is what makes a real texture set
// usable: every CC0 library ships its .mtlx with image nodes and no constants,
// so a loader that only read constants returned a flat colour and silently
// dropped every map. A connection this build cannot follow is warned about by
// name rather than dropped in silence.
//
// False on a read/parse error or a document with no surface shader; `out` keeps
// its defaults for whatever the document omits.
RX_ASSET_EXPORT bool LoadMaterialX(const std::string& path, Material* out,
                                   MaterialXMaps* maps = nullptr);

}  // namespace rx::asset

#endif  // RX_ASSET_MATERIALX_H_
