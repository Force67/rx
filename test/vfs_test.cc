// Vfs mount-point + .rxp pack acceptance test, pure CPU. Exercises virtual
// path parsing, the root (legacy) namespace, named and nested mount points,
// override order, the pack writer/reader round trip (store + deflate + the
// incompressible fallback), checksum tamper detection, and enumeration.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "asset/asset_id.h"
#include "asset/pack.h"
#include "asset/vfs.h"

namespace fs = std::filesystem;
namespace asset = rx::asset;

namespace {

int failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "vfs_test: FAIL %s:%d: %s\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

void WriteFile(const fs::path& path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string AsString(const base::Vector<u8>& bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void TestSplitVirtualPath() {
  asset::VirtualPath vp = asset::SplitVirtualPath("game://textures/a.dds");
  CHECK(vp.mount == "game" && vp.path == "textures/a.dds");
  vp = asset::SplitVirtualPath("textures/a.dds");
  CHECK(vp.mount.empty() && vp.path == "textures/a.dds");
  vp = asset::SplitVirtualPath("game://");
  CHECK(vp.mount == "game" && vp.path.empty());
  vp = asset::SplitVirtualPath("://x");  // not a mount name
  CHECK(vp.mount.empty() && vp.path == "://x");
  vp = asset::SplitVirtualPath("a/b://c");  // '/' in the would-be mount name
  CHECK(vp.mount.empty() && vp.path == "a/b://c");
}

void TestMounts(const fs::path& tmp) {
  const fs::path base_dir = tmp / "base";
  const fs::path patch_dir = tmp / "patch";
  const fs::path dlc_dir = tmp / "dlc";
  WriteFile(base_dir / "textures/rock.dds", "base rock");
  WriteFile(base_dir / "readme.txt", "base readme");
  WriteFile(patch_dir / "textures/rock.dds", "patched rock");
  WriteFile(dlc_dir / "extra.txt", "dlc extra");

  asset::Vfs vfs;

  // Legacy root namespace with override order: later mounts win.
  vfs.Mount(asset::MakeLooseFileProvider(base_dir.string()));
  vfs.Mount(asset::MakeLooseFileProvider(patch_dir.string()));
  auto rock = vfs.Read("Textures\\Rock.DDS");  // normalization on the way in
  CHECK(rock && AsString(*rock) == "patched rock");
  auto readme = vfs.Read("readme.txt");
  CHECK(readme && AsString(*readme) == "base readme");

  // Named mount, addressed by scheme; the scheme is case-insensitive.
  vfs.Mount("game", asset::MakeLooseFileProvider(base_dir.string()));
  auto via_scheme = vfs.Read("GAME://Readme.TXT");
  CHECK(via_scheme && AsString(*via_scheme) == "base readme");
  CHECK(vfs.Contains("game://textures/rock.dds"));
  CHECK(!vfs.Contains("game://extra.txt"));
  CHECK(!vfs.Contains("nosuch://readme.txt"));

  // Nested mount point: the provider's files appear under game://dlc/.
  vfs.Mount("game://dlc/", asset::MakeLooseFileProvider(dlc_dir.string()));
  auto extra = vfs.Read("game://dlc/extra.txt");
  CHECK(extra && AsString(*extra) == "dlc extra");
  CHECK(vfs.Contains("game://dlc/extra.txt"));
  CHECK(!vfs.Contains("dlc/extra.txt"));  // not in the root namespace

  // A later mount on the same scheme overrides the earlier one.
  vfs.Mount("game://", asset::MakeLooseFileProvider(patch_dir.string()));
  auto patched = vfs.Read("game://textures/rock.dds");
  CHECK(patched && AsString(*patched) == "patched rock");

  // Size without reading.
  CHECK(vfs.Size("game://dlc/extra.txt") == std::optional<rx::u64>(9));
  CHECK(!vfs.Size("game://dlc/none.txt"));

  // Enumerate emits full virtual paths; root entries stay schemeless.
  bool saw_root = false, saw_dlc = false, saw_game = false;
  vfs.Enumerate([&](std::string_view path) {
    if (path == "readme.txt") saw_root = true;
    if (path == "game://dlc/extra.txt") saw_dlc = true;
    if (path == "game://readme.txt") saw_game = true;
  });
  CHECK(saw_root && saw_dlc && saw_game);

  bool mount_scoped_dlc = false, mount_scoped_root = false;
  vfs.EnumerateMount("game", [&](std::string_view path) {
    if (path == "game://dlc/extra.txt") mount_scoped_dlc = true;
    if (path == "readme.txt") mount_scoped_root = true;
  });
  CHECK(mount_scoped_dlc && !mount_scoped_root);

  // Unmount by mount point.
  CHECK(vfs.Unmount("game://dlc/") == 1);
  CHECK(!vfs.Contains("game://dlc/extra.txt"));
  const size_t before = vfs.mount_count();
  CHECK(vfs.UnmountByPrefix(patch_dir.string()) == 2);  // root + game:// copies
  CHECK(vfs.mount_count() == before - 2);
  auto unpatched = vfs.Read("game://textures/rock.dds");
  CHECK(unpatched && AsString(*unpatched) == "base rock");
}

void TestPackRoundTrip(const fs::path& tmp) {
  fs::create_directories(tmp);
  const fs::path archive = tmp / "test.rxp";

  std::string compressible(64 * 1024, 'a');
  for (size_t i = 0; i < compressible.size(); i += 7) compressible[i] = 'b';

  base::Vector<u8> incompressible(4096);
  std::mt19937 rng(1234);
  for (size_t i = 0; i < incompressible.size(); ++i)
    incompressible[i] = static_cast<u8>(rng());

  asset::PackWriter writer;
  writer.Add("Meshes\\Rock.NIF", base::Vector<u8>(), asset::PackCompression::kStore);
  {
    base::Vector<u8> bytes(compressible.size());
    std::memcpy(bytes.data(), compressible.data(), compressible.size());
    writer.Add("textures/big.dds", std::move(bytes));
  }
  writer.Add("sound/noise.bin", std::move(incompressible));  // deflate must fall back
  {
    base::Vector<u8> bytes(5);
    std::memcpy(bytes.data(), "wrong", 5);
    writer.Add("dup.txt", std::move(bytes));
  }
  {
    base::Vector<u8> bytes(5);
    std::memcpy(bytes.data(), "right", 5);
    writer.Add("DUP.txt", std::move(bytes));  // replaces: same normalized path
  }
  CHECK(writer.entry_count() == 4);
  CHECK(writer.WriteTo(archive.string()));

  base::UniquePointer<asset::PackFile> pack = asset::PackFile::Open(archive.string());
  CHECK(pack);
  if (!pack) return;
  CHECK(pack->entry_count() == 4);

  // Entries land under their normalized paths.
  std::optional<size_t> rock = pack->Find("meshes/rock.nif");
  CHECK(rock);
  if (rock) {
    CHECK(pack->entry(*rock).size == 0);
    auto bytes = pack->ReadEntry(*rock);
    CHECK(bytes && bytes->empty());
  }

  std::optional<size_t> big = pack->Find("textures/big.dds");
  CHECK(big);
  if (big) {
    const asset::PackEntryView entry = pack->entry(*big);
    CHECK(entry.compression == asset::PackCompression::kDeflate);
    CHECK(entry.stored_size < entry.size);
    auto bytes = pack->ReadEntry(*big);
    CHECK(bytes && AsString(*bytes) == compressible);
  }

  std::optional<size_t> noise = pack->Find("sound/noise.bin");
  CHECK(noise);
  if (noise) {
    const asset::PackEntryView entry = pack->entry(*noise);
    CHECK(entry.compression == asset::PackCompression::kStore);
    CHECK(entry.stored_size == entry.size);
  }

  std::optional<size_t> dup = pack->Find("dup.txt");
  CHECK(dup);
  if (dup) {
    auto bytes = pack->ReadEntry(*dup);
    CHECK(bytes && AsString(*bytes) == "right");
  }
  CHECK(!pack->Find("nosuch.txt"));

  // Served through the Vfs under a named mount.
  asset::Vfs vfs;
  auto provider = asset::MakePackFileProvider(archive.string());
  CHECK(provider);
  vfs.Mount("pak", std::move(provider));
  auto via_vfs = vfs.Read("pak://dup.txt");
  CHECK(via_vfs && AsString(*via_vfs) == "right");
  CHECK(vfs.Size("pak://textures/big.dds") == std::optional<rx::u64>(compressible.size()));
}

void TestPackTamper(const fs::path& tmp) {
  fs::create_directories(tmp);
  const fs::path archive = tmp / "tamper.rxp";
  asset::PackWriter writer;
  {
    base::Vector<u8> bytes(11);
    std::memcpy(bytes.data(), "hello world", 11);
    writer.Add("a.txt", std::move(bytes), asset::PackCompression::kStore);
  }
  CHECK(writer.WriteTo(archive.string()));

  // Flip one payload byte: the entry checksum must catch it.
  {
    std::fstream f(archive, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(-1, std::ios::end);
    f.put('X');
  }
  base::UniquePointer<asset::PackFile> pack = asset::PackFile::Open(archive.string());
  CHECK(pack);
  if (pack) {
    std::optional<size_t> index = pack->Find("a.txt");
    CHECK(index && !pack->ReadEntry(*index));
  }

  // Corrupt the table of contents: Open must refuse the archive.
  {
    std::fstream f(archive, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(56);  // inside the first TOC entry
    f.put('X');
  }
  CHECK(!asset::PackFile::Open(archive.string()));

  CHECK(!asset::PackFile::Open((tmp / "missing.rxp").string()));
}

}  // namespace

int main() {
  const fs::path tmp = fs::temp_directory_path() / "rx_vfs_test";
  fs::remove_all(tmp);
  fs::create_directories(tmp);

  TestSplitVirtualPath();
  TestMounts(tmp / "mounts");
  TestPackRoundTrip(tmp / "pack");
  TestPackTamper(tmp / "tamper");

  fs::remove_all(tmp);
  if (failures) {
    std::fprintf(stderr, "vfs_test: %d failures\n", failures);
    return 1;
  }
  std::printf("vfs_test: OK\n");
  return 0;
}
