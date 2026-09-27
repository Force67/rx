#include "asset/content_mounts.h"

#include <base/option.h>

#include "asset/pack.h"
#include "base/memory/move.h"
#include "core/app_identity.h"
#include "core/file_system.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/sort.h"

namespace rx::asset {
namespace {

base::Option<const char*> ContentDir{"content.dir", nullptr, "RX_CONTENT_DIR"};
base::Option<const char*> EngineArchivesDir{"engine.archives", nullptr, "RX_ENGINE_ARCHIVES"};

// Everything the engine ships, one archive: a game adds its own beside it.
constexpr const char* kEngineArchive = "rx_engine.rxp";

bool MountPack(Vfs& vfs, const base::String& path, base::StringRef mount_point) {
  base::UniquePointer<FileProvider> provider = MakePackFileProvider(path);
  if (!provider) {
    RX_WARN("archive {} could not be opened", path);
    return false;
  }
  vfs.Mount(mount_point, base::move(provider));
  RX_INFO("mounted {} at {}", path, mount_point);
  return true;
}

}  // namespace

base::String ContentDirectory() {
  if (const char* dir = ContentDir.get(); dir && *dir) return dir;
  return ExecutableDirectory();
}

size_t MountContent(Vfs& vfs, base::StringRef name) {
  const base::String root = ContentDirectory();
  const base::String data = fs::Join(root, "Data");
  const base::String game = name.empty() ? base::String() : base::String(name) + "://";
  size_t mounted = 0;

  const char* engine_dir = EngineArchivesDir.get();
  const base::String engine_pack =
      fs::Join(engine_dir && *engine_dir ? base::String(engine_dir) : data, kEngineArchive);
  if (fs::IsRegularFile(engine_pack))
    mounted += MountPack(vfs, engine_pack, "rxe://");
  else
    RX_WARN("engine archive {} not found (Data/ beside the executable)", engine_pack);

  base::Vector<fs::DirEntry> entries;
  if (fs::ListDirectory(data, &entries)) {
    // Name order is mount order, and later mounts win: 01_base.rxp < 02_patch.rxp.
    rx::StableSort(entries.data(), entries.data() + entries.size(),
                   [](const fs::DirEntry& a, const fs::DirEntry& b) { return a.path < b.path; });
    for (const fs::DirEntry& entry : entries) {
      const base::StringRef name = fs::Filename(entry.path);
      if (!entry.is_regular || fs::Extension(entry.path) != ".rxp" || name == kEngineArchive)
        continue;
      mounted += MountPack(vfs, entry.path, game);
    }
  }

  // Loose engine files only where an engine build ships them; a game carries
  // the engine in its archive.
  if (const base::String engine_loose = fs::Join(root, "rxe"); fs::IsDirectory(engine_loose)) {
    vfs.Mount("rxe://", MakeLooseFileProvider(engine_loose));
    ++mounted;
  }
  vfs.Mount(game, MakeLooseFileProvider(root));
  vfs.Mount("user://", MakeLooseFileProvider(UserConfigDirectory()));
  return mounted + 2;
}

}  // namespace rx::asset
