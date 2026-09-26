#ifndef RX_ASSET_VFS_H_
#define RX_ASSET_VFS_H_


#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "base/functional/function_ref.h"
#include "base/optional.h"
#include "base/strings/string_ref.h"
#include "base/strings/xstring.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::asset {

// A source of files: a loose directory, an .rxp pack, a BSA, a BA2. Providers
// are mounted into the Vfs in priority order.
class FileProvider {
 public:
  virtual ~FileProvider() = default;

  virtual bool Contains(base::StringRef normalized_path) const = 0;
  virtual base::Optional<base::Vector<u8>> Read(base::StringRef normalized_path) const = 0;
  virtual void Enumerate(base::FunctionRef<void(base::StringRef)> fn) const = 0;
  virtual base::String name() const = 0;

  // Uncompressed size in bytes. The default reads the whole file; providers
  // with a table of contents (packs, archives) answer from metadata.
  virtual base::Optional<u64> Size(base::StringRef normalized_path) const;
};

// A virtual path split at its mount point:
//   "game://textures/a.dds" -> {"game", "textures/a.dds"}
//   "textures/a.dds"        -> {"",     "textures/a.dds"}   (the root mount)
// Views alias the input string; neither part is normalized yet.
struct VirtualPath {
  base::StringRef mount;
  base::StringRef path;
};
RX_ASSET_EXPORT VirtualPath SplitVirtualPath(base::StringRef path);

// Unified virtual filesystem over named mount points. Mount points:
//   "" / "game" / "game://"   the root or a named namespace
//   "game://dlc/"             a subtree: the provider's "a.dds" is game://dlc/a.dds
// Later mounts win: mount base archives, then DLC, then mods in plugin order,
// then loose files, reproducing the override behaviour mods rely on.
//
// Threading: Read/Contains/Size only read the mount table and are concurrent-
// safe (providers guard their own IO). Mount/Unmount/UnmountByPrefix mutate it
// unguarded: quiesce background loads first, or a loader reads a freed provider.
class RX_ASSET_EXPORT Vfs {
 public:
  void Mount(base::StringRef mount_point, base::UniquePointer<FileProvider> provider);

  // Legacy: mounts into the root namespace, reachable by schemeless paths.
  void Mount(base::UniquePointer<FileProvider> provider);

  // Removes every provider mounted at exactly `mount_point`, returning how many
  // were dropped.
  size_t Unmount(base::StringRef mount_point);

  // Removes every mounted provider whose name starts with `prefix`, returning how
  // many were dropped. Lets a caller swap one set of providers (reloaded mods)
  // for another without disturbing the rest of the stack. Single-threaded with
  // Read, like Mount.
  size_t UnmountByPrefix(base::StringRef prefix);

  // Accept full virtual paths ("game://a/b.dds") or schemeless root paths.
  base::Optional<base::Vector<u8>> Read(base::StringRef path) const;
  bool Contains(base::StringRef path) const;
  base::Optional<u64> Size(base::StringRef path) const;

  // Visits every entry across all mounted providers as a full virtual path
  // (root-mount entries stay schemeless, so pre-mount-point callers see the
  // same strings as before), with duplicates when an override shadows a base
  // file. For prefix/suffix discovery like finding the terrain LOD quads of a
  // worldspace.
  void Enumerate(base::FunctionRef<void(base::StringRef)> fn) const;

  // Same, restricted to the providers mounted under `mount_point`.
  void EnumerateMount(base::StringRef mount_point,
                      base::FunctionRef<void(base::StringRef)> fn) const;

  size_t mount_count() const { return mounts_.size(); }

 private:
  struct MountEntry {
    base::String mount;   // normalized mount name, "" = root
    base::String prefix;  // normalized subtree prefix, "" or "dlc/.../" with trailing slash
    base::UniquePointer<FileProvider> provider;
  };

  const FileProvider* Resolve(base::StringRef path, base::String* relative) const;

  base::Vector<MountEntry> mounts_;
};

RX_ASSET_EXPORT base::UniquePointer<FileProvider> MakeLooseFileProvider(
    base::String root_directory);

}  // namespace rx::asset

#endif  // RX_ASSET_VFS_H_
