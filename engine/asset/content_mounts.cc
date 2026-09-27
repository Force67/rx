#include "asset/content_mounts.h"

#include <base/option.h>

#include "asset/pack.h"
#include "base/memory/move.h"
#include "core/file_system.h"
#include "core/log.h"
#include "core/paths.h"
#include "core/sort.h"

namespace rx::asset {
namespace {

base::Option<const char*> ContentDir{"content.dir", nullptr, "RX_CONTENT_DIR"};
base::Option<const char*> EngineArchivesDir{"engine.archives", nullptr, "RX_ENGINE_ARCHIVES"};

// The engine's archives: one per namespace under rxe://.
struct EngineArchive {
  const char* file_name;
  const char* mount_point;
};
constexpr EngineArchive kEngineArchives[] = {{"rx_fonts.rxp", "rxe://fonts/"}};

bool IsEngineArchive(base::StringRef file_name) {
  for (const EngineArchive& archive : kEngineArchives)
    if (file_name == archive.file_name) return true;
  return false;
}

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

size_t MountContent(Vfs& vfs, base::StringRef title) {
  const base::String root = ContentDirectory();
  const base::String data = fs::Join(root, "Data");
  const base::String game = title.empty() ? base::String() : base::String(title) + "://";
  size_t mounted = 0;

  const char* engine_dir = EngineArchivesDir.get();
  const base::String engine_data = engine_dir && *engine_dir ? base::String(engine_dir) : data;
  for (const EngineArchive& archive : kEngineArchives) {
    const base::String path = fs::Join(engine_data, archive.file_name);
    if (!fs::IsRegularFile(path)) {
      RX_WARN("engine archive {} not found (Data/ beside the executable)", path);
      continue;
    }
    mounted += MountPack(vfs, path, archive.mount_point);
  }

  base::Vector<fs::DirEntry> entries;
  if (fs::ListDirectory(data, &entries)) {
    // Name order is mount order, and later mounts win: 01_base.rxp < 02_patch.rxp.
    rx::StableSort(entries.data(), entries.data() + entries.size(),
                   [](const fs::DirEntry& a, const fs::DirEntry& b) { return a.path < b.path; });
    for (const fs::DirEntry& entry : entries) {
      const base::StringRef name = fs::Filename(entry.path);
      if (!entry.is_regular || fs::Extension(entry.path) != ".rxp" || IsEngineArchive(name))
        continue;
      mounted += MountPack(vfs, entry.path, game);
    }
  }

  const base::String engine_loose = fs::Join(root, "rxe");
  if (fs::IsDirectory(engine_loose)) {
    vfs.Mount("rxe://", MakeLooseFileProvider(engine_loose));
    ++mounted;
  } else {
    RX_WARN("engine directory {} not found: no rxe://config", engine_loose);
  }
  vfs.Mount(game, MakeLooseFileProvider(root));
  return mounted + 1;
}

}  // namespace rx::asset
