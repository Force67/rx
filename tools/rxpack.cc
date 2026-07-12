// rxpack — authoring tool for .rxp game archives (engine/asset/pack.h).
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

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "asset/asset_id.h"
#include "asset/pack.h"

namespace fs = std::filesystem;
namespace asset = rx::asset;

namespace {

int Usage() {
  std::fprintf(stderr,
               "usage: rxpack create <archive.rxp> <input_dir> [--store] [--level N]\n"
               "       rxpack list <archive.rxp>\n"
               "       rxpack extract <archive.rxp> <output_dir> [virtual paths...]\n"
               "       rxpack verify <archive.rxp>\n");
  return 2;
}

int Fail(const std::string& msg) {
  std::fprintf(stderr, "rxpack: %s\n", msg.c_str());
  return 1;
}

std::optional<base::Vector<u8>> ReadFileBytes(const fs::path& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return std::nullopt;
  base::Vector<u8> data(static_cast<size_t>(file.tellg()));
  file.seekg(0);
  file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!file) return std::nullopt;
  return data;
}

int Create(int argc, char** argv) {
  if (argc < 4) return Usage();
  const std::string archive = argv[2];
  const fs::path input_dir = argv[3];
  asset::PackCompression compression = asset::PackCompression::kDeflate;
  asset::PackWriter writer;
  for (int i = 4; i < argc; ++i) {
    if (std::strcmp(argv[i], "--store") == 0) {
      compression = asset::PackCompression::kStore;
    } else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
      writer.set_compression_level(std::atoi(argv[++i]));
    } else {
      return Usage();
    }
  }
  if (!fs::is_directory(input_dir)) return Fail("not a directory: " + input_dir.string());

  u64 total_in = 0;
  for (const auto& entry : fs::recursive_directory_iterator(input_dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string virtual_path =
        asset::NormalizePath(fs::relative(entry.path(), input_dir).generic_string());
    std::optional<base::Vector<u8>> bytes = ReadFileBytes(entry.path());
    if (!bytes) return Fail("cannot read " + entry.path().string());
    total_in += bytes->size();
    writer.Add(virtual_path, std::move(*bytes), compression);
  }
  if (writer.entry_count() == 0) return Fail("no files under " + input_dir.string());
  if (!writer.WriteTo(archive)) return Fail("cannot write " + archive);

  std::error_code ec;
  const u64 total_out = fs::file_size(archive, ec);
  std::printf("%s: %zu entries, %llu -> %llu bytes (%.1f%%)\n", archive.c_str(),
              writer.entry_count(), static_cast<unsigned long long>(total_in),
              static_cast<unsigned long long>(total_out),
              total_in ? 100.0 * static_cast<double>(total_out) / static_cast<double>(total_in)
                       : 100.0);
  return 0;
}

int List(int argc, char** argv) {
  if (argc != 3) return Usage();
  base::UniquePointer<asset::PackFile> pack = asset::PackFile::Open(argv[2]);
  if (!pack) return Fail(std::string("cannot open ") + argv[2]);
  u64 total_size = 0, total_stored = 0;
  std::printf("%12s %12s  %-7s %s\n", "size", "stored", "method", "path");
  for (size_t i = 0; i < pack->entry_count(); ++i) {
    const asset::PackEntryView entry = pack->entry(i);
    total_size += entry.size;
    total_stored += entry.stored_size;
    std::printf("%12llu %12llu  %-7s %.*s\n", static_cast<unsigned long long>(entry.size),
                static_cast<unsigned long long>(entry.stored_size),
                entry.compression == asset::PackCompression::kDeflate ? "deflate" : "store",
                static_cast<int>(entry.path.size()), entry.path.data());
  }
  std::printf("%12llu %12llu  %zu entries\n", static_cast<unsigned long long>(total_size),
              static_cast<unsigned long long>(total_stored), pack->entry_count());
  return 0;
}

int Extract(int argc, char** argv) {
  if (argc < 4) return Usage();
  base::UniquePointer<asset::PackFile> pack = asset::PackFile::Open(argv[2]);
  if (!pack) return Fail(std::string("cannot open ") + argv[2]);
  const fs::path out_dir = argv[3];

  base::Vector<size_t> indices;
  if (argc == 4) {
    for (size_t i = 0; i < pack->entry_count(); ++i) indices.push_back(i);
  } else {
    for (int i = 4; i < argc; ++i) {
      const std::optional<size_t> index = pack->Find(asset::NormalizePath(argv[i]));
      if (!index) return Fail(std::string("no such entry: ") + argv[i]);
      indices.push_back(*index);
    }
  }

  for (size_t index : indices) {
    const asset::PackEntryView entry = pack->entry(index);
    std::optional<base::Vector<u8>> bytes = pack->ReadEntry(index);
    if (!bytes) return Fail("corrupt entry: " + std::string(entry.path));
    const fs::path out_path = out_dir / fs::path(entry.path);
    std::error_code ec;
    fs::create_directories(out_path.parent_path(), ec);
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes->data()),
              static_cast<std::streamsize>(bytes->size()));
    if (!out) return Fail("cannot write " + out_path.string());
  }
  std::printf("extracted %zu entries to %s\n", indices.size(), out_dir.string().c_str());
  return 0;
}

int Verify(int argc, char** argv) {
  if (argc != 3) return Usage();
  base::UniquePointer<asset::PackFile> pack = asset::PackFile::Open(argv[2]);
  if (!pack) return Fail(std::string("cannot open ") + argv[2]);
  size_t bad = 0;
  for (size_t i = 0; i < pack->entry_count(); ++i) {
    if (!pack->ReadEntry(i)) {
      std::fprintf(stderr, "rxpack: CORRUPT %s\n", std::string(pack->entry(i).path).c_str());
      ++bad;
    }
  }
  if (bad) return Fail(std::to_string(bad) + " corrupt entries");
  std::printf("%s: %zu entries OK\n", argv[2], pack->entry_count());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return Usage();
  if (std::strcmp(argv[1], "create") == 0) return Create(argc, argv);
  if (std::strcmp(argv[1], "list") == 0) return List(argc, argv);
  if (std::strcmp(argv[1], "extract") == 0) return Extract(argc, argv);
  if (std::strcmp(argv[1], "verify") == 0) return Verify(argc, argv);
  return Usage();
}
