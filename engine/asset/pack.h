#ifndef RX_ASSET_PACK_H_
#define RX_ASSET_PACK_H_

#include <mutex>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <base/containers/vector.h>
#include <base/memory/unique_pointer.h>

#include "asset/vfs.h"
#include "core/export.h"
#include "core/types.h"

namespace rx::asset {

// .rxp — the engine's game archive ("rx pack"), in the spirit of RAGE's RPF:
// a flat table of contents keyed by the fnv1a-64 hash of each normalized
// virtual path (binary-searchable, full paths kept in a name table), payloads
// stored raw or deflated, each guarded by a CRC-32 of the uncompressed bytes.
//
// Layout, little-endian:
//   PackHeader                               48 bytes
//   PackEntry[entry_count]                   48 bytes each, sorted by (hash, path)
//   name table                               null-terminated normalized paths
//   payload region                           16-byte aligned blobs
//
// Authored by PackWriter / the rxpack tool; served at runtime through
// MakePackFileProvider, mounted into the Vfs like any other provider.

enum class PackCompression : u8 {
  kStore = 0,
  kDeflate = 1,  // zlib-wrapped DEFLATE (miniz)
};

struct PackEntryView {
  std::string_view path;  // normalized virtual path
  u64 size = 0;           // uncompressed
  u64 stored_size = 0;    // bytes occupied in the archive
  PackCompression compression = PackCompression::kStore;
  u32 crc32 = 0;          // of the uncompressed payload
};

// Read side: the table of contents lives in memory, payloads are fetched (and
// verified against their checksum) on demand. ReadEntry is thread-safe.
class RX_ASSET_EXPORT PackFile {
 public:
  // Null when the file is missing, truncated, has a bad magic/version, or its
  // table of contents fails the checksum.
  static base::UniquePointer<PackFile> Open(std::string archive_path);

  size_t entry_count() const { return entry_count_; }
  PackEntryView entry(size_t index) const;

  // `normalized_path` as produced by NormalizePath (lowercase, forward slashes).
  std::optional<size_t> Find(std::string_view normalized_path) const;

  // Decompresses and checksum-verifies; nullopt on any mismatch or IO failure.
  std::optional<base::Vector<u8>> ReadEntry(size_t index) const;

  const std::string& path() const { return path_; }

  PackFile(const PackFile&) = delete;
  PackFile& operator=(const PackFile&) = delete;

 private:
  PackFile() = default;

  std::string path_;
  size_t entry_count_ = 0;
  base::Vector<u8> toc_;      // raw PackEntry records
  base::Vector<char> names_;  // name table backing entry() string views
  mutable std::mutex io_mutex_;
  mutable std::ifstream file_;
};

// Write side: stage payloads in memory, then emit the archive in one pass.
// kDeflate quietly keeps the raw bytes when compression does not shrink them.
class RX_ASSET_EXPORT PackWriter {
 public:
  // `virtual_path` is normalized on the way in; adding the same path twice
  // replaces the earlier payload.
  void Add(std::string_view virtual_path, base::Vector<u8> bytes,
           PackCompression compression = PackCompression::kDeflate);

  // miniz level 1..10; default 6 balances ratio and packing time.
  void set_compression_level(int level) { compression_level_ = level; }

  size_t entry_count() const { return pending_.size(); }

  bool WriteTo(const std::string& file_path);

 private:
  struct Pending {
    std::string path;
    base::Vector<u8> bytes;
    PackCompression compression;
  };

  base::Vector<Pending> pending_;
  int compression_level_ = 6;
};

// Serves an .rxp archive as a Vfs provider. Null when the archive fails to open.
RX_ASSET_EXPORT base::UniquePointer<FileProvider> MakePackFileProvider(std::string archive_path);

}  // namespace rx::asset

#endif  // RX_ASSET_PACK_H_
