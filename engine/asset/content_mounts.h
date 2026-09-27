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

// Mounts the install layout beside the executable and the app's user folder
// (docs/CONFIG.md), archives before loose files so a loose file overrides the
// packed one:
//
//   Data/rx_engine.rxp -> rxe://      everything the engine ships (fonts/, config/)
//   Data/*.rxp         -> <name>://   the game's archives, in name order
//   rxe/               -> rxe://      loose engine files, where an engine build
//                                     ships them (the viewer, the editor)
//   ./                 -> <name>://   the game's loose files (config/, ...)
//   UserConfigDirectory() -> user://  the player's settings (config/, controls.ini)
//
// RX_ENGINE_ARCHIVES names another directory to take rx_engine.rxp from. Returns
// the number of providers mounted.
RX_ASSET_EXPORT size_t MountContent(Vfs& vfs, base::StringRef name);

}  // namespace rx::asset

#endif  // RX_ASSET_CONTENT_MOUNTS_H_
