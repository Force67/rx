#include "asset/asset_database.h"

#include <mutex>

#include "core/log.h"

namespace rx::asset {
namespace {

base::String ExtensionOf(std::string_view normalized_path) {
  size_t dot = normalized_path.rfind('.');
  if (dot == std::string_view::npos)
    return {};
  std::string_view extension = normalized_path.substr(dot);
  return base::String(extension.data(), extension.size());
}

template <typename Asset, typename Converter>
const Asset *
LoadWith(std::mutex &mutex, Vfs &vfs, std::string_view path,
         const base::UnorderedMap<base::String, Converter> &converters,
         base::UnorderedMap<u64, base::UniquePointer<Asset>> &cache) {
  std::string normalized = NormalizePath(path);
  AssetId id = MakeAssetId(normalized);
  // Record the id -> path mapping so tooling (scene serialization) can write a
  // relocatable path for this asset even without a database handle. Done before
  // the cache check so it survives repeated lookups and cached failures.
  RecordAssetPath(id, normalized);
  {
    std::scoped_lock lock(mutex);
    if (auto *cached = cache.find(id.hash))
      return cached->Get_UseOnlyIfYouKnowWhatYouareDoing();
  }

  // Convert outside the lock: converters recurse into LoadTexture/AddMaterial
  // (which re-lock), and holding it through a whole convert would stall every
  // other loader on this expensive step.
  base::UniquePointer<Asset> asset;
  const char *failure = nullptr;
  const Converter *converter = converters.find(ExtensionOf(normalized));
  if (!converter) {
    failure = "no converter for";
  } else if (auto bytes = vfs.Read(normalized); !bytes) {
    failure = "asset not found";
  } else {
    asset = (*converter)(ByteSpan(bytes->data(), bytes->size()), id, normalized);
    if (!asset)
      failure = "conversion failed";
  }
  if (failure)
    RX_WARN("{}: {}", failure, normalized);

  // Another thread may have converted the same key meanwhile; the existing
  // entry wins and our duplicate is dropped. Failures cache a null entry so
  // repeated lookups stay cheap.
  std::scoped_lock lock(mutex);
  return cache.emplace(id.hash, std::move(asset))
      .first->Get_UseOnlyIfYouKnowWhatYouareDoing();
}

template <typename Asset>
const Asset *
FindIn(const base::UnorderedMap<u64, base::UniquePointer<Asset>> &cache,
       AssetId id) {
  const auto *cached = cache.find(id.hash);
  return cached ? cached->Get_UseOnlyIfYouKnowWhatYouareDoing() : nullptr;
}

} // namespace

void AssetDatabase::RegisterMeshConverter(base::String extension,
                                          MeshConverter converter) {
  mesh_converters_.emplace(extension, std::move(converter));
}

void AssetDatabase::RegisterTextureConverter(base::String extension,
                                             TextureConverter converter) {
  texture_converters_.emplace(extension, std::move(converter));
}

void AssetDatabase::RegisterMaterialConverter(base::String extension,
                                              MaterialConverter converter) {
  material_converters_.emplace(extension, std::move(converter));
}

const Mesh *AssetDatabase::LoadMesh(std::string_view path) {
  return LoadWith(mutex_, vfs_, path, mesh_converters_, meshes_);
}

const Texture *AssetDatabase::LoadTexture(std::string_view path) {
  return LoadWith(mutex_, vfs_, path, texture_converters_, textures_);
}

const Material *AssetDatabase::LoadMaterial(std::string_view path) {
  return LoadWith(mutex_, vfs_, path, material_converters_, materials_);
}

void AssetDatabase::AddMaterial(const Material &material) {
  std::scoped_lock lock(mutex_);
  if (materials_.contains(material.id.hash))
    return;
  materials_.emplace(material.id.hash, base::MakeUnique<Material>(material));
}

const Mesh *AssetDatabase::AddMesh(Mesh mesh) {
  u64 hash = mesh.id.hash;
  std::scoped_lock lock(mutex_);
  if (const auto *existing = meshes_.find(hash)) {
    return existing->Get_UseOnlyIfYouKnowWhatYouareDoing();
  }
  return meshes_.emplace(hash, base::MakeUnique<Mesh>(std::move(mesh)))
      .first->Get_UseOnlyIfYouKnowWhatYouareDoing();
}

const Mesh *AssetDatabase::ReplaceMesh(Mesh mesh) {
  const u64 hash = mesh.id.hash;
  std::scoped_lock lock(mutex_);
  if (auto *existing = meshes_.find(hash)) {
    Mesh *value = existing->Get_UseOnlyIfYouKnowWhatYouareDoing();
    if (value) {
      *value = std::move(mesh);
      return value;
    }
    meshes_.erase(hash);
  }
  return meshes_.emplace(hash, base::MakeUnique<Mesh>(std::move(mesh)))
      .first->Get_UseOnlyIfYouKnowWhatYouareDoing();
}

bool AssetDatabase::RemoveMesh(AssetId id) {
  std::scoped_lock lock(mutex_);
  return meshes_.erase(id.hash) != 0;
}

const Texture *AssetDatabase::AddTexture(Texture texture) {
  u64 hash = texture.id.hash;
  std::scoped_lock lock(mutex_);
  if (const auto *existing = textures_.find(hash)) {
    return existing->Get_UseOnlyIfYouKnowWhatYouareDoing();
  }
  return textures_.emplace(hash, base::MakeUnique<Texture>(std::move(texture)))
      .first->Get_UseOnlyIfYouKnowWhatYouareDoing();
}

const Material *AssetDatabase::FindMaterial(AssetId id) const {
  std::scoped_lock lock(mutex_);
  return FindIn(materials_, id);
}

Material *AssetDatabase::FindMaterialMutable(AssetId id) {
  std::scoped_lock lock(mutex_);
  auto *cached = materials_.find(id.hash);
  return cached ? cached->Get_UseOnlyIfYouKnowWhatYouareDoing() : nullptr;
}

const Texture *AssetDatabase::FindTexture(AssetId id) const {
  std::scoped_lock lock(mutex_);
  return FindIn(textures_, id);
}

const Mesh *AssetDatabase::FindMesh(AssetId id) const {
  std::scoped_lock lock(mutex_);
  return FindIn(meshes_, id);
}

} // namespace rx::asset
