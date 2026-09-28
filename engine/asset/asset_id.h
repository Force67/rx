#ifndef RX_ASSET_ASSET_ID_H_
#define RX_ASSET_ASSET_ID_H_


#include "base/optional.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "foundation/build_config/export.h"
#include "foundation/build_config/types.h"

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
  // Spelled out rather than a defaulted <=>, which needs <compare>.
  bool operator<(const AssetId& o) const { return hash < o.hash; }
  bool operator<=(const AssetId& o) const { return hash <= o.hash; }
  bool operator>(const AssetId& o) const { return hash > o.hash; }
  bool operator>=(const AssetId& o) const { return hash >= o.hash; }
  explicit operator bool() const { return hash != 0; }
};

RX_ASSET_EXPORT AssetId MakeAssetId(base::StringRef normalized_path);

// Lowercases and converts backslashes to forward slashes (e.g. Bethesda-style
// backslash asset paths normalize the same as forward-slash ones).
RX_ASSET_EXPORT base::String NormalizePath(base::StringRef path);

// Process-global id -> source-path table for tooling (scene serialization needs
// to write a human-readable, relocatable path for a Renderable's AssetId rather
// than a raw hash). The AssetDatabase records every path it is asked to load,
// so a saver with no database handle can still resolve ids it produced. The
// path stored is already normalized; recording is idempotent per id.
RX_ASSET_EXPORT void RecordAssetPath(AssetId id, base::StringRef normalized_path);
RX_ASSET_EXPORT base::Optional<base::String> LookupAssetPath(AssetId id);

}  // namespace rx::asset

#endif  // RX_ASSET_ASSET_ID_H_
