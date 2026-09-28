#include "base/containers/array.h"
#include "base/memory/move.h"
#include "base/optional.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "base/threading/thread.h"
#include "rxe/asset/asset_database.h"

#include <base/memory/unique_pointer.h>

#include <stdio.h>

namespace asset = rx::asset;

namespace {

int failures = 0;

#define CHECK(cond)                                                                             \
  do {                                                                                          \
    if (!(cond)) {                                                                              \
      ::fprintf(stderr, "asset_database_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                                               \
    }                                                                                           \
  } while (0)

class MemoryProvider final : public asset::FileProvider {
 public:
  bool Contains(base::StringRef) const override { return true; }

  base::Optional<base::Vector<rx::u8>> Read(base::StringRef) const override {
    base::Vector<rx::u8> bytes;
    bytes.push_back(1);
    return bytes;
  }

  void Enumerate(base::FunctionRef<void(base::StringRef)>) const override {}
  base::String name() const override { return "memory"; }
};

void TestAddSupersedesFailure() {
  asset::Vfs vfs;
  asset::AssetDatabase db(vfs);

  const asset::AssetId mesh_id = asset::MakeAssetId("missing.mesh");
  CHECK(db.LoadMesh("missing.mesh") == nullptr);
  asset::Mesh mesh;
  mesh.id = mesh_id;
  CHECK(db.AddMesh(base::move(mesh)) != nullptr);
  CHECK(db.FindMesh(mesh_id) != nullptr);

  const asset::AssetId texture_id = asset::MakeAssetId("missing.tex");
  CHECK(db.LoadTexture("missing.tex") == nullptr);
  asset::Texture texture;
  texture.id = texture_id;
  CHECK(db.AddTexture(base::move(texture)) != nullptr);
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

  db.RegisterTextureConverter(".tex", [](rx::ByteSpan, asset::AssetId id, base::StringRef) {
    auto texture = base::MakeUnique<asset::Texture>();
    texture->id = id;
    return texture;
  });
  db.RegisterMeshConverter(".mesh", [&db](rx::ByteSpan, asset::AssetId id, base::StringRef) {
    if (!db.LoadTexture("shared.tex")) return base::UniquePointer<asset::Mesh>();
    asset::Material material;
    material.id = asset::MakeAssetId("shared.mat");
    db.AddMaterial(material);
    auto mesh = base::MakeUnique<asset::Mesh>();
    mesh->id = id;
    return mesh;
  });

  base::Array<const asset::Mesh*, 8> results{};
  base::Array<base::UniquePointer<base::Thread>, 8> threads;
  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i] = base::MakeUnique<base::Thread>(
        "asset_load", [&db, &results, i] { results[i] = db.LoadMesh("shared.mesh"); },
        /*start_now=*/true);
  }
  for (base::UniquePointer<base::Thread>& thread : threads) thread->Join();

  for (const asset::Mesh* result : results) CHECK(result == results[0]);
  CHECK(results[0] != nullptr);
  CHECK(db.FindTexture(asset::MakeAssetId("shared.tex")) != nullptr);
  CHECK(db.FindMaterial(asset::MakeAssetId("shared.mat")) != nullptr);
}

}  // namespace

int main() {
  TestAddSupersedesFailure();
  TestConcurrentRecursiveLoad();
  if (failures == 0) ::printf("asset_database_test: PASS\n");
  return failures == 0 ? 0 : 1;
}
