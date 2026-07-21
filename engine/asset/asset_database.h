#ifndef RX_ASSET_ASSET_DATABASE_H_
#define RX_ASSET_ASSET_DATABASE_H_

#include <functional>
#include <mutex>

#include <base/containers/unordered_map.h>
#include <base/memory/unique_pointer.h>
#include <base/strings/xstring.h>

#include "asset/asset_id.h"
#include "asset/material.h"
#include "asset/mesh.h"
#include "asset/texture.h"
#include "asset/vfs.h"
#include "core/export.h"

namespace rx::asset {

// Converts raw bytes from the Vfs into an engine asset. The application
// registers converters for its formats (e.g. a Bethesda module registers .nif,
// .dds, .bgsm and friends). Keyed by extension so new formats plug in without
// touching this module. The normalized source path rides along for converters
// that key behavior off naming conventions (e.g. _n.dds normal maps stay
// linear).
using MeshConverter = std::function<base::UniquePointer<Mesh>(
    ByteSpan, AssetId, std::string_view path)>;
using TextureConverter = std::function<base::UniquePointer<Texture>(
    ByteSpan, AssetId, std::string_view path)>;
using MaterialConverter = std::function<base::UniquePointer<Material>(
    ByteSpan, AssetId, std::string_view path)>;

class RX_ASSET_EXPORT AssetDatabase {
public:
  explicit AssetDatabase(Vfs &vfs) : vfs_(vfs) {}

  void RegisterMeshConverter(base::String extension, MeshConverter converter);
  void RegisterTextureConverter(base::String extension,
                                TextureConverter converter);
  void RegisterMaterialConverter(base::String extension,
                                 MaterialConverter converter);

  // Loads (converting on first use) or returns the cached asset. Failures
  // cache as null so missing files are only probed once. Safe to call from
  // multiple threads: the caches are mutex-guarded, and conversion runs
  // outside the lock (converters recurse into LoadTexture/AddMaterial, and a
  // background prefetch must not stall the main thread for a whole convert).
  // Concurrent loads of the same key may both convert; the first insert wins,
  // except that a successful convert supersedes a concurrently cached failure.
  const Mesh *LoadMesh(std::string_view path);
  const Texture *LoadTexture(std::string_view path);
  const Material *LoadMaterial(std::string_view path);

  // Side channel for converters that synthesize assets while converting
  // another (NIF shader properties become materials) and for procedurally
  // built meshes/textures (terrain). Keyed by their id.
  void AddMaterial(const Material &material);
  const Mesh *AddMesh(Mesh mesh);
  // Explicit mutation path for authored procedural assets. Replaces in place
  // when present, preserving pointers held by streaming shells.
  const Mesh *ReplaceMesh(Mesh mesh);
  bool RemoveMesh(AssetId id);
  const Texture *AddTexture(Texture texture);
  const Material *FindMaterial(AssetId id) const;
  // Mutable handle for late tweaks to a synthesized material before it uploads
  // (e.g. flagging a grass model's materials for vertex wind).
  Material *FindMaterialMutable(AssetId id);
  const Texture *FindTexture(AssetId id) const;
  const Mesh *FindMesh(AssetId id) const;

  Vfs &vfs() { return vfs_; }

private:
  Vfs &vfs_;
  // Registration happens at startup before any load; the converter maps are
  // read-only afterwards and stay unguarded.
  base::UnorderedMap<base::String, MeshConverter> mesh_converters_;
  base::UnorderedMap<base::String, TextureConverter> texture_converters_;
  base::UnorderedMap<base::String, MaterialConverter> material_converters_;
  // Guards the three asset caches. Handed-out pointers stay valid: values are
  // heap objects behind UniquePointers, stable across rehash. Invariant:
  // background prefetch threads only ADD (LoadMesh/LoadTexture); Remove* and
  // Replace* stay main-thread-only and must not race a convert on the same key.
  mutable std::mutex mutex_;
  base::UnorderedMap<u64, base::UniquePointer<Mesh>> meshes_;
  base::UnorderedMap<u64, base::UniquePointer<Texture>> textures_;
  base::UnorderedMap<u64, base::UniquePointer<Material>> materials_;
};

} // namespace rx::asset

#endif // RX_ASSET_ASSET_DATABASE_H_
