#include "asset/engine_archives.h"


#include <base/option.h>

#include "asset/pack.h"
#include "base/memory/move.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/file_system.h"
#include "core/log.h"
#include "core/paths.h"

namespace rx::asset {
namespace {

// Directory holding the engine's .rxp archives. Empty means: look in the
// working directory, then where the build put them.
base::Option<const char*> EngineArchivesDir{"engine.archives", nullptr, "RX_ENGINE_ARCHIVES"};

// One archive per namespace; see engine_archives.h.
struct EngineArchive {
  const char* file_name;
  const char* mount_point;
};
constexpr EngineArchive kEngineArchives[] = {{"rx_fonts.rxp", "fonts"}};

}  // namespace

base::String FindEngineArchive(base::StringRef file_name) {
  auto try_dir = [&](base::StringRef dir) {
    base::String candidate = fs::Join(dir, file_name);
    return fs::IsRegularFile(candidate) ? candidate : base::String();
  };

  if (const char* dir = EngineArchivesDir.get(); dir != nullptr && *dir != '\0') {
    if (base::String found = try_dir(dir); !found.empty()) return found;
  }
  if (base::String found = try_dir("."); !found.empty()) return found;
  // A shipped build carries the archives beside the executable, and is started
  // from wherever the launcher happens to be.
  if (base::String found = try_dir(ExecutableDirectory()); !found.empty()) return found;
#ifdef RX_ENGINE_ARCHIVES_DIR_DEFAULT
  if (base::String found = try_dir(RX_ENGINE_ARCHIVES_DIR_DEFAULT); !found.empty()) return found;
#endif
  return {};
}

size_t MountEngineArchives(Vfs& vfs) {
  size_t mounted = 0;
  for (const EngineArchive& archive : kEngineArchives) {
    const base::String path = FindEngineArchive(archive.file_name);
    if (path.empty()) {
      RX_WARN("engine archive {} not found (set RX_ENGINE_ARCHIVES)", archive.file_name);
      continue;
    }
    base::UniquePointer<FileProvider> provider = MakePackFileProvider(path);
    if (!provider) {
      RX_WARN("engine archive {} could not be opened", path);
      continue;
    }
    vfs.Mount(archive.mount_point, base::move(provider));
    ++mounted;
    RX_INFO("mounted {} at {}://", path, archive.mount_point);
  }
  return mounted;
}

}  // namespace rx::asset
