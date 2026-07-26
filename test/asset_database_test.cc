#include "asset/asset_database.h"

#include <base/memory/unique_pointer.h>

#include <array>
#include <cstdio>
#include <string>
#include <thread>

namespace asset = rx::asset;

namespace {

int failures = 0;

#define CHECK(cond)                                                                             \
  do {                                                                                          \
    if (!(cond)) {                                                                              \
      std::fprintf(stderr, "asset_database_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                               \
    }                                                                                           \
  } while (0)

class MemoryProvider final : public asset::FileProvider {
 public:
  bool Contains(std::string_view) const override { return true; }

  std::optional<base::Vector<rx::u8>> Read(std::string_view) const override {
    base::Vector<rx::u8> bytes;
    bytes.push_back(1);
    return bytes;
  }

  void Enumerate(const std::function<void(std::string_view)>&) const override {}
  std::string name() const override { return "memory"; }
};

void TestAddSupersedesFailure() {
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);

  const asset::AssetId mesh_id = asset::MakeAssetId("missing.mesh");
  CHECK(db.LoadMesh("missing.mesh") == nullptr);
  asset::Mesh mesh;
  mesh.id = mesh_id;
  CHECK(db.AddMesh(std::move(mesh)) != nullptr);
  CHECK(db.FindMesh(mesh_id) != nullptr);

  const asset::AssetId texture_id = asset::MakeAssetId("missing.tex");
  CHECK(db.LoadTexture("missing.tex") == nullptr);
  asset::Texture texture;
  texture.id = texture_id;
  CHECK(db.AddTexture(std::move(texture)) != nullptr);
  CHECK(db.FindTexture(texture_id) != nullptr);

  const asset::AssetId material_id = asset::MakeAssetId("missing.mat");
  CHECK(db.LoadMaterial("missing.mat") == nullptr);
  asset::Material material;
  material.id = material_id;
  db.AddMaterial(material);
  CHECK(db.FindMaterial(material_id) != nullptr);
}

void TestConcurrentRecursiveLoad() {
  asset::Vfs vfs;
  vfs.Mount(base::MakeUnique<MemoryProvider>());
  asset::AssetDatabase db(vfs);

  db.RegisterTextureConverter(".tex", [](rx::ByteSpan, asset::AssetId id, std::string_view) {
    auto texture = base::MakeUnique<asset::Texture>();
    texture->id = id;
    return texture;
  });
  db.RegisterMeshConverter(".mesh", [&db](rx::ByteSpan, asset::AssetId id, std::string_view) {
    if (!db.LoadTexture("shared.tex")) return base::UniquePointer<asset::Mesh>();
    asset::Material material;
    material.id = asset::MakeAssetId("shared.mat");
    db.AddMaterial(material);
    auto mesh = base::MakeUnique<asset::Mesh>();
    mesh->id = id;
    return mesh;
  });

  std::array<const asset::Mesh*, 8> results{};
  std::array<std::thread, 8> threads;
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i] = std::thread([&db, &results, i] { results[i] = db.LoadMesh("shared.mesh"); });
  }
  for (std::thread& thread : threads) thread.join();

  for (const asset::Mesh* result : results) CHECK(result == results[0]);
  CHECK(results[0] != nullptr);
  CHECK(db.FindTexture(asset::MakeAssetId("shared.tex")) != nullptr);
  CHECK(db.FindMaterial(asset::MakeAssetId("shared.mat")) != nullptr);
}

}  // namespace

int main() {
  TestAddSupersedesFailure();
  TestConcurrentRecursiveLoad();
  if (failures == 0) std::printf("asset_database_test: PASS\n");
  return failures == 0 ? 0 : 1;
}
