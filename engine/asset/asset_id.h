#ifndef RX_ASSET_ASSET_ID_H_
#define RX_ASSET_ASSET_ID_H_

#include <string_view>

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

// Stable id derived from the normalized virtual path. Converted Bethesda
// assets keep their source path, so "meshes/clutter/bucket01.nif" hashes the
// same no matter which archive or loose directory provided it.
struct AssetId {
  u64 hash = 0;

  bool operator==(const AssetId&) const = default;
  auto operator<=>(const AssetId&) const = default;
  explicit operator bool() const { return hash != 0; }
};

AssetId MakeAssetId(std::string_view normalized_path);

// Lowercases and converts backslashes, matching Bethesda path conventions.
std::string NormalizePath(std::string_view path);

}  // namespace rx::asset

#endif  // RX_ASSET_ASSET_ID_H_
