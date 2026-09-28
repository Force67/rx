#include "asset/vfs.h"

#include <ctype.h>

#include <base/containers/unordered_map.h>

#include "asset/asset_id.h"
#include "base/memory/move.h"
#include "base/optional.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "base/threading/lock_guard.h"
#include "base/threading/mutex.h"
#include "foundation/files/file_system.h"

namespace rx::asset {

AssetId MakeAssetId(base::StringRef normalized_path) { return AssetId{Fnv1a(normalized_path)}; }

namespace {

// Guarded by a mutex: recording happens on the load path (potentially the job
// system), lookups from tooling on the main thread. Small and cold, so a plain
// mutex is fine.
struct PathTable {
  base::Mutex mutex;
  base::UnorderedMap<u64, base::String> paths;
};

PathTable& ThePathTable() {
  static PathTable table;
  return table;
}

}  // namespace

void RecordAssetPath(AssetId id, base::StringRef normalized_path) {
  if (!id) return;
  PathTable& table = ThePathTable();
  base::LockGuard<base::Mutex> lock(table.mutex);
  if (base::String* existing = table.paths.find(id.hash))
    *existing = base::String(normalized_path);
  else
    table.paths.emplace(id.hash, base::String(normalized_path));
}

base::Optional<base::String> LookupAssetPath(AssetId id) {
  PathTable& table = ThePathTable();
  base::LockGuard<base::Mutex> lock(table.mutex);
  if (const base::String* found = table.paths.find(id.hash)) return *found;
  return base::nullopt;
}

base::String NormalizePath(base::StringRef path) {
  base::String out(path);
  for (char& c : out) {
    c = c == '\\' ? '/' : static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

VirtualPath SplitVirtualPath(base::StringRef path) {
  const size_t pos = path.find("://");
  // A mount name is a bare identifier: "a/b://c" or "://c" is not a mount.
  if (pos == base::StringRef::npos || pos == 0 ||
      path.substr(0, pos).find_first_of("/\\") != base::StringRef::npos) {
    return {base::StringRef(), path};
  }
  return {path.substr(0, pos), path.substr(pos + 3)};
}

base::Optional<u64> FileProvider::Size(base::StringRef normalized_path) const {
  const base::Optional<base::Vector<u8>> bytes = Read(normalized_path);
  if (!bytes) return base::nullopt;
  return bytes->size();
}

namespace {

// A mount point ("", "game", "game://", "game://dlc/") split into its
// normalized mount name and subtree prefix (trailing slash enforced).
struct MountPoint {
  base::String mount;
  base::String prefix;
};

MountPoint ParseMountPoint(base::StringRef mount_point) {
  const VirtualPath split = SplitVirtualPath(mount_point);
  MountPoint out;
  if (split.mount.empty()) {
    // "game" (no ://) names a whole scheme; "" is the root namespace.
    out.mount = NormalizePath(mount_point);
  } else {
    out.mount = NormalizePath(split.mount);
    out.prefix = NormalizePath(split.path);
    if (!out.prefix.empty() && out.prefix.back() != '/') out.prefix.push_back('/');
  }
  return out;
}

}  // namespace

void Vfs::Mount(base::StringRef mount_point, base::UniquePointer<FileProvider> provider) {
  MountPoint at = ParseMountPoint(mount_point);
  mounts_.push_back(MountEntry{base::move(at.mount), base::move(at.prefix), base::move(provider)});
}

void Vfs::Mount(base::UniquePointer<FileProvider> provider) {
  Mount(base::StringRef(), base::move(provider));
}

size_t Vfs::Unmount(base::StringRef mount_point) {
  const MountPoint at = ParseMountPoint(mount_point);
  base::Vector<MountEntry> kept;
  size_t removed = 0;
  for (size_t i = 0; i < mounts_.size(); ++i) {
    if (mounts_[i].mount == at.mount && mounts_[i].prefix == at.prefix) {
      ++removed;
    } else {
      kept.push_back(base::move(mounts_[i]));
    }
  }
  mounts_ = base::move(kept);
  return removed;
}

size_t Vfs::UnmountByPrefix(base::StringRef prefix) {
  base::Vector<MountEntry> kept;
  size_t removed = 0;
  for (size_t i = 0; i < mounts_.size(); ++i) {
    const base::String name = mounts_[i].provider->name();
    if (base::StringRef(name).starts_with(prefix)) {
      ++removed;
    } else {
      kept.push_back(base::move(mounts_[i]));
    }
  }
  mounts_ = base::move(kept);
  return removed;
}

const FileProvider* Vfs::Resolve(base::StringRef path, base::String* relative) const {
  const VirtualPath split = SplitVirtualPath(path);
  const base::String mount = NormalizePath(split.mount);
  const base::String normalized = NormalizePath(split.path);
  for (size_t i = mounts_.size(); i-- > 0;) {
    const MountEntry& entry = mounts_[i];
    if (entry.mount != mount) continue;
    if (!normalized.starts_with(entry.prefix)) continue;
    base::String sub = normalized.substr(entry.prefix.size());
    if (!entry.provider->Contains(sub)) continue;
    *relative = base::move(sub);
    return &*entry.provider;
  }
  return nullptr;
}

base::Optional<base::Vector<u8>> Vfs::Read(base::StringRef path) const {
  base::String relative;
  if (const FileProvider* provider = Resolve(path, &relative)) return provider->Read(relative);
  return base::nullopt;
}

bool Vfs::Contains(base::StringRef path) const {
  base::String relative;
  return Resolve(path, &relative) != nullptr;
}

base::Optional<u64> Vfs::Size(base::StringRef path) const {
  base::String relative;
  if (const FileProvider* provider = Resolve(path, &relative)) return provider->Size(relative);
  return base::nullopt;
}

namespace {

void EnumerateEntryAsVirtualPaths(const base::String& mount, const base::String& prefix,
                                  const FileProvider& provider,
                                  base::FunctionRef<void(base::StringRef)> fn) {
  if (mount.empty() && prefix.empty()) {
    // Root mount: schemeless, exactly the provider's own paths.
    provider.Enumerate(fn);
    return;
  }
  base::String full;
  provider.Enumerate([&](base::StringRef path) {
    full.clear();
    if (!mount.empty()) {
      full += mount;
      full += "://";
    }
    full += prefix;
    full += path;
    fn(full);
  });
}

}  // namespace

void Vfs::Enumerate(base::FunctionRef<void(base::StringRef)> fn) const {
  for (const MountEntry& entry : mounts_)
    EnumerateEntryAsVirtualPaths(entry.mount, entry.prefix, *entry.provider, fn);
}

void Vfs::EnumerateMount(base::StringRef mount_point,
                         base::FunctionRef<void(base::StringRef)> fn) const {
  const MountPoint at = ParseMountPoint(mount_point);
  for (const MountEntry& entry : mounts_) {
    if (entry.mount != at.mount || !entry.prefix.starts_with(at.prefix)) continue;
    EnumerateEntryAsVirtualPaths(entry.mount, entry.prefix, *entry.provider, fn);
  }
}

namespace {

class LooseFileProvider final : public FileProvider {
 public:
  explicit LooseFileProvider(base::String root) : root_(base::move(root)) {}

  bool Contains(base::StringRef normalized_path) const override {
    return fs::IsRegularFile(fs::Join(root_, normalized_path));
  }

  base::Optional<base::Vector<u8>> Read(base::StringRef normalized_path) const override {
    base::Vector<u8> data;
    if (!fs::ReadFile(fs::Join(root_, normalized_path), &data)) return base::nullopt;
    return data;
  }

  base::Optional<u64> Size(base::StringRef normalized_path) const override {
    return fs::FileSize(fs::Join(root_, normalized_path));
  }

  void Enumerate(base::FunctionRef<void(base::StringRef)> fn) const override {
    base::Vector<fs::DirEntry> entries;
    fs::ListDirectory(root_, &entries, /*recursive=*/true);
    for (const fs::DirEntry& entry : entries) {
      if (!entry.is_regular) continue;
      fn(NormalizePath(fs::GenericString(fs::Relative(entry.path, root_))));
    }
  }

  base::String name() const override { return root_; }

 private:
  base::String root_;
};

}  // namespace

base::UniquePointer<FileProvider> MakeLooseFileProvider(base::String root_directory) {
  return base::MakeUnique<LooseFileProvider>(base::move(root_directory));
}

}  // namespace rx::asset
