// rxpack, authoring tool for .rxp game archives (engine/asset/pack.h).
//
//   rxpack create <archive.rxp> <input_dir> [--store] [--level N]
//   rxpack list <archive.rxp>
//   rxpack extract <archive.rxp> <output_dir> [virtual paths...]
//   rxpack verify <archive.rxp>
//
// create packs a directory tree; every file is added under its normalized
// relative path and deflated unless --store is given (incompressible payloads
// fall back to raw storage automatically). extract writes entries back out as
// loose files. verify decompresses every entry against its checksum.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_id.h"
#include "asset/pack.h"
#include "base/memory/move.h"
#include "base/optional.h"
#include "base/strings/xstring.h"
#include "foundation/files/file_system.h"
#include "foundation/strings/format.h"

namespace fs = rx::fs;
namespace asset = rx::asset;

namespace {

int Usage() {
  ::fprintf(stderr,
               "usage: rxpack create <archive.rxp> <input_dir> [--store] [--level N]\n"
               "       rxpack list <archive.rxp>\n"
               "       rxpack extract <archive.rxp> <output_dir> [virtual paths...]\n"
               "       rxpack verify <archive.rxp>\n");
  return 2;
}

int Fail(const base::String& msg) {
  ::fprintf(stderr, "rxpack: %s\n", msg.c_str());
  return 1;
}

base::Optional<base::Vector<u8>> ReadFileBytes(base::StringRef path) {
  base::Vector<u8> data;
  if (!fs::ReadFile(path, &data)) return base::nullopt;
  return data;
}

int Create(int argc, char** argv) {
  if (argc < 4) return Usage();
  const base::String archive = argv[2];
  const base::String input_dir = argv[3];
  asset::PackCompression compression = asset::PackCompression::kDeflate;
  asset::PackWriter writer;
  for (int i = 4; i < argc; ++i) {
    if (::strcmp(argv[i], "--store") == 0) {
      compression = asset::PackCompression::kStore;
    } else if (::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
      writer.set_compression_level(::atoi(argv[++i]));
    } else {
      return Usage();
    }
  }
  if (!fs::IsDirectory(input_dir)) return Fail("not a directory: " + input_dir);

  u64 total_in = 0;
  base::Vector<fs::DirEntry> entries;
  if (!fs::ListDirectory(input_dir, &entries, /*recursive=*/true))
    return Fail("cannot list " + input_dir);
  for (const fs::DirEntry& entry : entries) {
    if (!entry.is_regular) continue;
    const base::String virtual_path =
        asset::NormalizePath(fs::GenericString(fs::Relative(entry.path, input_dir)));
    base::Optional<base::Vector<u8>> bytes = ReadFileBytes(entry.path);
    if (!bytes) return Fail("cannot read " + entry.path);
    total_in += bytes->size();
    writer.Add(virtual_path, base::move(*bytes), compression);
  }
  if (writer.entry_count() == 0) return Fail("no files under " + input_dir);
  if (!writer.WriteTo(archive)) return Fail("cannot write " + archive);

  const u64 total_out = fs::FileSize(archive).value_or(0);
  ::printf("%s: %zu entries, %llu -> %llu bytes (%.1f%%)\n", archive.c_str(),
              writer.entry_count(), static_cast<unsigned long long>(total_in),
              static_cast<unsigned long long>(total_out),
              total_in ? 100.0 * static_cast<double>(total_out) / static_cast<double>(total_in)
                       : 100.0);
  return 0;
}

int List(int argc, char** argv) {
  if (argc != 3) return Usage();
  base::UniquePointer<asset::PackFile> pack = asset::PackFile::Open(argv[2]);
  if (!pack) return Fail(base::String("cannot open ") + argv[2]);
  u64 total_size = 0, total_stored = 0;
  ::printf("%12s %12s  %-7s %s\n", "size", "stored", "method", "path");
  for (size_t i = 0; i < pack->entry_count(); ++i) {
    const asset::PackEntryView entry = pack->entry(i);
    total_size += entry.size;
    total_stored += entry.stored_size;
    ::printf("%12llu %12llu  %-7s %.*s\n", static_cast<unsigned long long>(entry.size),
                static_cast<unsigned long long>(entry.stored_size),
                entry.compression == asset::PackCompression::kDeflate ? "deflate" : "store",
                static_cast<int>(entry.path.size()), entry.path.data());
  }
  ::printf("%12llu %12llu  %zu entries\n", static_cast<unsigned long long>(total_size),
              static_cast<unsigned long long>(total_stored), pack->entry_count());
  return 0;
}

int Extract(int argc, char** argv) {
  if (argc < 4) return Usage();
  base::UniquePointer<asset::PackFile> pack = asset::PackFile::Open(argv[2]);
  if (!pack) return Fail(base::String("cannot open ") + argv[2]);
  const base::String out_dir = argv[3];

  base::Vector<size_t> indices;
  if (argc == 4) {
    for (size_t i = 0; i < pack->entry_count(); ++i) indices.push_back(i);
  } else {
    for (int i = 4; i < argc; ++i) {
      const base::Optional<size_t> index = pack->Find(asset::NormalizePath(argv[i]));
      if (!index) return Fail(base::String("no such entry: ") + argv[i]);
      indices.push_back(*index);
    }
  }

  for (size_t index : indices) {
    const asset::PackEntryView entry = pack->entry(index);
    base::Optional<base::Vector<u8>> bytes = pack->ReadEntry(index);
    if (!bytes) return Fail("corrupt entry: " + base::String(entry.path));
    const base::String out_path = fs::Join(out_dir, entry.path);
    fs::CreateDirectories(fs::ParentPath(out_path));
    if (!fs::WriteFile(out_path, base::Span<const u8>(bytes->data(), bytes->size())))
      return Fail("cannot write " + out_path);
  }
  ::printf("extracted %zu entries to %s\n", indices.size(), out_dir.c_str());
  return 0;
}

int Verify(int argc, char** argv) {
  if (argc != 3) return Usage();
  base::UniquePointer<asset::PackFile> pack = asset::PackFile::Open(argv[2]);
  if (!pack) return Fail(base::String("cannot open ") + argv[2]);
  size_t bad = 0;
  for (size_t i = 0; i < pack->entry_count(); ++i) {
    if (!pack->ReadEntry(i)) {
      ::fprintf(stderr, "rxpack: CORRUPT %s\n", base::String(pack->entry(i).path).c_str());
      ++bad;
    }
  }
  if (bad) return Fail(rx::ToString(bad) + " corrupt entries");
  ::printf("%s: %zu entries OK\n", argv[2], pack->entry_count());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return Usage();
  if (::strcmp(argv[1], "create") == 0) return Create(argc, argv);
  if (::strcmp(argv[1], "list") == 0) return List(argc, argv);
  if (::strcmp(argv[1], "extract") == 0) return Extract(argc, argv);
  if (::strcmp(argv[1], "verify") == 0) return Verify(argc, argv);
  return Usage();
}
