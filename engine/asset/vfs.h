#ifndef RX_ASSET_VFS_H_
#define RX_ASSET_VFS_H_

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "core/export.h"
#include "core/types.h"

namespace rx::asset {

// A source of files: a loose directory, an .rxp pack, a BSA, a BA2. Providers
// are mounted into the Vfs in priority order.
class FileProvider {
 public:
  virtual ~FileProvider() = default;

  virtual bool Contains(std::string_view normalized_path) const = 0;
  virtual std::optional<base::Vector<u8>> Read(std::string_view normalized_path) const = 0;
  virtual void Enumerate(const std::function<void(std::string_view)>& fn) const = 0;
  virtual std::string name() const = 0;

  // Uncompressed size in bytes. The default reads the whole file; providers
  // with a table of contents (packs, archives) answer from metadata.
  virtual std::optional<u64> Size(std::string_view normalized_path) const;
};

// A virtual path split at its mount point:
//   "game://textures/a.dds" -> {"game", "textures/a.dds"}
//   "textures/a.dds"        -> {"",     "textures/a.dds"}   (the root mount)
// Views alias the input string; neither part is normalized yet.
struct VirtualPath {
  std::string_view mount;
  std::string_view path;
};
RX_ASSET_EXPORT VirtualPath SplitVirtualPath(std::string_view path);

// Unified virtual filesystem over named mount points. A provider mounts under
// a mount point of the form
//   ""              the root (schemeless) namespace; legacy single-arg Mount
//   "game"          everything addressed as game://...
//   "game://"       same
//   "game://dlc/"   a subtree: the provider's "a.dds" resolves as game://dlc/a.dds
// so archives and loose directories from anywhere unify under one namespace.
//
// Later mounts win. Mount base game archives first, then DLC, then mod
// archives in plugin order, then loose files last. This reproduces the
// override behavior mods rely on.
class RX_ASSET_EXPORT Vfs {
 public:
  void Mount(std::string_view mount_point, base::UniquePointer<FileProvider> provider);

  // Legacy: mounts into the root namespace, reachable by schemeless paths.
  void Mount(base::UniquePointer<FileProvider> provider);

  // Removes every provider mounted at exactly `mount_point`, returning how many
  // were dropped.
  size_t Unmount(std::string_view mount_point);

  // Removes every mounted provider whose name starts with `prefix`, returning how
  // many were dropped. Lets a caller swap one set of providers (reloaded mods)
  // for another without disturbing the rest of the stack. Single-threaded with
  // Read, like Mount.
  size_t UnmountByPrefix(std::string_view prefix);

  // Accept full virtual paths ("game://a/b.dds") or schemeless root paths.
  std::optional<base::Vector<u8>> Read(std::string_view path) const;
  bool Contains(std::string_view path) const;
  std::optional<u64> Size(std::string_view path) const;

  // Visits every entry across all mounted providers as a full virtual path
  // (root-mount entries stay schemeless, so pre-mount-point callers see the
  // same strings as before), with duplicates when an override shadows a base
  // file. For prefix/suffix discovery like finding the terrain LOD quads of a
  // worldspace.
  void Enumerate(const std::function<void(std::string_view)>& fn) const;

  // Same, restricted to the providers mounted under `mount_point`.
  void EnumerateMount(std::string_view mount_point,
                      const std::function<void(std::string_view)>& fn) const;

  size_t mount_count() const { return mounts_.size(); }

 private:
  struct MountEntry {
    std::string mount;   // normalized mount name, "" = root
    std::string prefix;  // normalized subtree prefix, "" or "dlc/.../" with trailing slash
    base::UniquePointer<FileProvider> provider;
  };

  const FileProvider* Resolve(std::string_view path, std::string* relative) const;

  base::Vector<MountEntry> mounts_;
};

base::UniquePointer<FileProvider> MakeLooseFileProvider(std::string root_directory);

}  // namespace rx::asset

#endif  // RX_ASSET_VFS_H_
