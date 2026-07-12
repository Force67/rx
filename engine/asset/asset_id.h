#ifndef RX_ASSET_ASSET_ID_H_
#define RX_ASSET_ASSET_ID_H_

#include <optional>
#include <string>
#include <string_view>

#include "core/export.h"
#include "core/types.h"

namespace rx::asset {

enum class AssetType : u8 {
  kMesh,
  kTexture,
  kMaterial,
  kAnimation,
  kSound,
  kScript,
};

// Stable id derived from the normalized virtual path. Converted assets keep
// their source path, so "meshes/clutter/bucket01.nif" hashes the same no matter
// which archive or loose directory provided it.
struct AssetId {
  u64 hash = 0;

  bool operator==(const AssetId&) const = default;
  auto operator<=>(const AssetId&) const = default;
  explicit operator bool() const { return hash != 0; }
};

RX_ASSET_EXPORT AssetId MakeAssetId(std::string_view normalized_path);

// Lowercases and converts backslashes to forward slashes (e.g. Bethesda-style
// backslash asset paths normalize the same as forward-slash ones).
RX_ASSET_EXPORT std::string NormalizePath(std::string_view path);

// Process-global id -> source-path table for tooling (scene serialization needs
// to write a human-readable, relocatable path for a Renderable's AssetId rather
// than a raw hash). The AssetDatabase records every path it is asked to load,
// so a saver with no database handle can still resolve ids it produced. The
// path stored is already normalized; recording is idempotent per id.
RX_ASSET_EXPORT void RecordAssetPath(AssetId id, std::string_view normalized_path);
RX_ASSET_EXPORT std::optional<std::string> LookupAssetPath(AssetId id);

}  // namespace rx::asset

#endif  // RX_ASSET_ASSET_ID_H_
