#ifndef RX_ASSET_BLEND_IMPORT_H_
#define RX_ASSET_BLEND_IMPORT_H_

#include <string>

#include "core/export.h"

namespace rx::asset {

struct BlendImportOptions {
  std::string blender_executable = "blender";
  // Absolute path to tools/blend_to_glb.py. Kept explicit so installed editors
  // can ship or replace the converter without baking a source-tree path into
  // the asset module.
  std::string converter_script;
  // Empty uses $XDG_CACHE_HOME/rx/blend (or the platform temp directory).
  std::string cache_directory;
  bool force = false;
};

struct BlendImportResult {
  std::string glb_path;
  std::string manifest_path;
  bool reused_cache = false;
};

// Converts a .blend authoring scene into a cached, game-oriented GLB by running
// Blender in background mode. Arguments are passed without a shell. Cache keys
// include source size/mtime and converter mtime, so changing either invalidates
// stale output. Returns false when Blender is missing or conversion fails.
RX_ASSET_EXPORT bool ConvertBlendScene(const std::string &blend_path,
                                       const BlendImportOptions &options,
                                       BlendImportResult *out,
                                       std::string *error = nullptr);

} // namespace rx::asset

#endif // RX_ASSET_BLEND_IMPORT_H_
