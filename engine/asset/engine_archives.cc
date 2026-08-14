#include "asset/engine_archives.h"

#include <filesystem>

#include <base/option.h>

#include "asset/pack.h"
#include "core/log.h"

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

std::string FindEngineArchive(std::string_view file_name) {
  std::error_code ec;
  auto try_dir = [&](const std::filesystem::path& dir) {
    const std::filesystem::path candidate = dir / file_name;
    return std::filesystem::is_regular_file(candidate, ec) ? candidate.string() : std::string();
  };

  if (const char* dir = EngineArchivesDir.get(); dir != nullptr && *dir != '\0') {
    if (std::string found = try_dir(dir); !found.empty()) return found;
  }
  if (std::string found = try_dir("."); !found.empty()) return found;
#ifdef RX_ENGINE_ARCHIVES_DIR_DEFAULT
  if (std::string found = try_dir(RX_ENGINE_ARCHIVES_DIR_DEFAULT); !found.empty()) return found;
#endif
  return {};
}

size_t MountEngineArchives(Vfs& vfs) {
  size_t mounted = 0;
  for (const EngineArchive& archive : kEngineArchives) {
    const std::string path = FindEngineArchive(archive.file_name);
    if (path.empty()) {
      RX_WARN("engine archive {} not found (set RX_ENGINE_ARCHIVES)", archive.file_name);
      continue;
    }
    base::UniquePointer<FileProvider> provider = MakePackFileProvider(path);
    if (!provider) {
      RX_WARN("engine archive {} could not be opened", path);
      continue;
    }
    vfs.Mount(archive.mount_point, std::move(provider));
    ++mounted;
    RX_INFO("mounted {} at {}://", path, archive.mount_point);
  }
  return mounted;
}

}  // namespace rx::asset
