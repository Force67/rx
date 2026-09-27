#ifndef RX_ASSET_CONTENT_MOUNTS_H_
#define RX_ASSET_CONTENT_MOUNTS_H_

#include "asset/vfs.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::asset {

// The directory a game's content sits in: the executable's, or RX_CONTENT_DIR
// when set (to run a binary against content somewhere else).
RX_ASSET_EXPORT base::String ContentDirectory();

// Mounts the install layout beside the executable (docs/CONFIG.md), archives
// before loose files so a loose file overrides the packed one:
//
//   Data/rx_fonts.rxp  -> rxe://fonts/   the engine's own archives, by name
//   Data/*.rxp         -> <title>://     every other archive, in name order
//   rxe/               -> rxe://         the engine's loose files (rxe/config/)
//   ./                 -> <title>://     the game's loose files (config/, ...)
//
// RX_ENGINE_ARCHIVES names another directory to take the engine's archives
// from. An empty title mounts the game's side at the root namespace. Returns the
// number of providers mounted.
RX_ASSET_EXPORT size_t MountContent(Vfs& vfs, base::StringRef title);

}  // namespace rx::asset

#endif  // RX_ASSET_CONTENT_MOUNTS_H_
