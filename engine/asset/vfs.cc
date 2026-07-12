#include "asset/vfs.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "asset/asset_id.h"

namespace rx::asset {

AssetId MakeAssetId(std::string_view normalized_path) { return AssetId{Fnv1a(normalized_path)}; }

std::string NormalizePath(std::string_view path) {
  std::string out(path);
  std::ranges::transform(out, out.begin(), [](char c) {
    if (c == '\\') return '/';
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return out;
}

VirtualPath SplitVirtualPath(std::string_view path) {
  const size_t pos = path.find("://");
  // A mount name is a bare identifier: "a/b://c" or "://c" is not a mount.
  if (pos == std::string_view::npos || pos == 0 ||
      path.substr(0, pos).find_first_of("/\\") != std::string_view::npos) {
    return {std::string_view(), path};
  }
  return {path.substr(0, pos), path.substr(pos + 3)};
}

std::optional<u64> FileProvider::Size(std::string_view normalized_path) const {
  const std::optional<base::Vector<u8>> bytes = Read(normalized_path);
  if (!bytes) return std::nullopt;
  return bytes->size();
}

namespace {

// A mount point ("", "game", "game://", "game://dlc/") split into its
// normalized mount name and subtree prefix (trailing slash enforced).
struct MountPoint {
  std::string mount;
  std::string prefix;
};

MountPoint ParseMountPoint(std::string_view mount_point) {
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

void Vfs::Mount(std::string_view mount_point, base::UniquePointer<FileProvider> provider) {
  MountPoint at = ParseMountPoint(mount_point);
  mounts_.push_back(MountEntry{std::move(at.mount), std::move(at.prefix), std::move(provider)});
}

void Vfs::Mount(base::UniquePointer<FileProvider> provider) {
  Mount(std::string_view(), std::move(provider));
}

size_t Vfs::Unmount(std::string_view mount_point) {
  const MountPoint at = ParseMountPoint(mount_point);
  base::Vector<MountEntry> kept;
  size_t removed = 0;
  for (size_t i = 0; i < mounts_.size(); ++i) {
    if (mounts_[i].mount == at.mount && mounts_[i].prefix == at.prefix) {
      ++removed;
    } else {
      kept.push_back(std::move(mounts_[i]));
    }
  }
  mounts_ = std::move(kept);
  return removed;
}

size_t Vfs::UnmountByPrefix(std::string_view prefix) {
  base::Vector<MountEntry> kept;
  size_t removed = 0;
  for (size_t i = 0; i < mounts_.size(); ++i) {
    const std::string name = mounts_[i].provider->name();
    if (name.size() >= prefix.size() &&
        name.compare(0, prefix.size(), prefix.data(), prefix.size()) == 0) {
      ++removed;
    } else {
      kept.push_back(std::move(mounts_[i]));
    }
  }
  mounts_ = std::move(kept);
  return removed;
}

const FileProvider* Vfs::Resolve(std::string_view path, std::string* relative) const {
  const VirtualPath split = SplitVirtualPath(path);
  const std::string mount = NormalizePath(split.mount);
  const std::string normalized = NormalizePath(split.path);
  for (size_t i = mounts_.size(); i-- > 0;) {
    const MountEntry& entry = mounts_[i];
    if (entry.mount != mount) continue;
    if (!normalized.starts_with(entry.prefix)) continue;
    std::string sub = normalized.substr(entry.prefix.size());
    if (!entry.provider->Contains(sub)) continue;
    *relative = std::move(sub);
    return &*entry.provider;
  }
  return nullptr;
}

std::optional<base::Vector<u8>> Vfs::Read(std::string_view path) const {
  std::string relative;
  if (const FileProvider* provider = Resolve(path, &relative)) return provider->Read(relative);
  return std::nullopt;
}

bool Vfs::Contains(std::string_view path) const {
  std::string relative;
  return Resolve(path, &relative) != nullptr;
}

std::optional<u64> Vfs::Size(std::string_view path) const {
  std::string relative;
  if (const FileProvider* provider = Resolve(path, &relative)) return provider->Size(relative);
  return std::nullopt;
}

namespace {

void EnumerateEntryAsVirtualPaths(const std::string& mount, const std::string& prefix,
                                  const FileProvider& provider,
                                  const std::function<void(std::string_view)>& fn) {
  if (mount.empty() && prefix.empty()) {
    // Root mount: schemeless, exactly the provider's own paths.
    provider.Enumerate(fn);
    return;
  }
  std::string full;
  provider.Enumerate([&](std::string_view path) {
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

void Vfs::Enumerate(const std::function<void(std::string_view)>& fn) const {
  for (const MountEntry& entry : mounts_)
    EnumerateEntryAsVirtualPaths(entry.mount, entry.prefix, *entry.provider, fn);
}

void Vfs::EnumerateMount(std::string_view mount_point,
                         const std::function<void(std::string_view)>& fn) const {
  const MountPoint at = ParseMountPoint(mount_point);
  for (const MountEntry& entry : mounts_) {
    if (entry.mount != at.mount || !entry.prefix.starts_with(at.prefix)) continue;
    EnumerateEntryAsVirtualPaths(entry.mount, entry.prefix, *entry.provider, fn);
  }
}

namespace {

class LooseFileProvider final : public FileProvider {
 public:
  explicit LooseFileProvider(std::string root) : root_(std::move(root)) {}

  bool Contains(std::string_view normalized_path) const override {
    return std::filesystem::is_regular_file(root_ / std::filesystem::path(normalized_path));
  }

  std::optional<base::Vector<u8>> Read(std::string_view normalized_path) const override {
    std::ifstream file(root_ / std::filesystem::path(normalized_path),
                       std::ios::binary | std::ios::ate);
    if (!file) return std::nullopt;
    base::Vector<u8> data(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return data;
  }

  std::optional<u64> Size(std::string_view normalized_path) const override {
    std::error_code ec;
    const auto size = std::filesystem::file_size(root_ / std::filesystem::path(normalized_path), ec);
    if (ec) return std::nullopt;
    return size;
  }

  void Enumerate(const std::function<void(std::string_view)>& fn) const override {
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root_, ec)) {
      if (!entry.is_regular_file()) continue;
      fn(NormalizePath(std::filesystem::relative(entry.path(), root_).generic_string()));
    }
  }

  std::string name() const override { return root_.string(); }

 private:
  std::filesystem::path root_;
};

}  // namespace

base::UniquePointer<FileProvider> MakeLooseFileProvider(std::string root_directory) {
  return base::MakeUnique<LooseFileProvider>(std::move(root_directory));
}

}  // namespace rx::asset
