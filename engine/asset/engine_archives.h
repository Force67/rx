#ifndef RX_ASSET_ENGINE_ARCHIVES_H_
#define RX_ASSET_ENGINE_ARCHIVES_H_

#include <string>
#include <string_view>

#include "asset/vfs.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::asset {

// rx's own content, shipped as .rxp archives next to the game's. One archive
// per namespace, mounted under the mount point it is named for:
//
//   rx_fonts.rxp -> fonts://    the built-in UI fonts (Roboto)
//
// They mount first, so a game archive of the same name overrides them.

// Where MountEngineArchives looks, in order: RX_ENGINE_ARCHIVES if set, the
// working directory, the directory the executable sits in, then the build
// directory the engine was compiled in.
RX_ASSET_EXPORT std::string FindEngineArchive(std::string_view file_name);

// Mounts every engine archive found. Returns how many mounted; 0 just means the
// engine runs without its built-in content (the callers all degrade).
RX_ASSET_EXPORT size_t MountEngineArchives(Vfs& vfs);

}  // namespace rx::asset

#endif  // RX_ASSET_ENGINE_ARCHIVES_H_
