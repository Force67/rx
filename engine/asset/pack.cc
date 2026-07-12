#include "asset/pack.h"

#include <algorithm>
#include <cstring>

#include <miniz.h>

#include "asset/asset_id.h"

namespace rx::asset {

namespace {

constexpr char kMagic[4] = {'R', 'X', 'P', 'K'};
constexpr u32 kVersion = 1;
constexpr u64 kPayloadAlignment = 16;

struct PackHeader {
  char magic[4];
  u32 version;
  u32 entry_count;
  u32 toc_crc32;  // over the entry records + name table
  u64 names_offset;
  u64 names_size;
  u64 data_offset;
  u64 reserved;
};
static_assert(sizeof(PackHeader) == 48);

struct PackEntry {
  u64 path_hash;  // Fnv1a of the normalized virtual path
  u64 offset;     // absolute byte offset of the stored payload
  u64 stored_size;
  u64 size;  // uncompressed
  u32 name_offset;
  u32 crc32;  // of the uncompressed payload
  u8 compression;
  u8 pad[7];
};
static_assert(sizeof(PackEntry) == 48);

u32 Crc32(const u8* data, size_t size) {
  return static_cast<u32>(mz_crc32(MZ_CRC32_INIT, data, size));
}

u64 AlignUp(u64 value, u64 alignment) { return (value + alignment - 1) & ~(alignment - 1); }

}  // namespace

// --- read side ---------------------------------------------------------------

base::UniquePointer<PackFile> PackFile::Open(std::string archive_path) {
  base::UniquePointer<PackFile> pack(new PackFile());
  pack->path_ = std::move(archive_path);
  pack->file_.open(pack->path_, std::ios::binary);
  if (!pack->file_) return nullptr;

  PackHeader header{};
  if (!pack->file_.read(reinterpret_cast<char*>(&header), sizeof(header))) return nullptr;
  if (std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0) return nullptr;
  if (header.version != kVersion) return nullptr;

  const u64 toc_size = u64{header.entry_count} * sizeof(PackEntry);
  if (header.names_offset != sizeof(PackHeader) + toc_size) return nullptr;
  if (header.data_offset < header.names_offset + header.names_size) return nullptr;

  pack->toc_.resize(toc_size);
  if (!pack->file_.read(reinterpret_cast<char*>(pack->toc_.data()),
                        static_cast<std::streamsize>(toc_size)))
    return nullptr;
  pack->names_.resize(header.names_size);
  if (!pack->file_.read(pack->names_.data(), static_cast<std::streamsize>(header.names_size)))
    return nullptr;

  mz_ulong crc = mz_crc32(MZ_CRC32_INIT, pack->toc_.data(), pack->toc_.size());
  crc = mz_crc32(crc, reinterpret_cast<const u8*>(pack->names_.data()), pack->names_.size());
  if (static_cast<u32>(crc) != header.toc_crc32) return nullptr;

  // Every name offset must land inside a terminated name table.
  if (header.names_size == 0 && header.entry_count > 0) return nullptr;
  if (header.names_size > 0 && pack->names_[pack->names_.size() - 1] != '\0') return nullptr;
  const PackEntry* entries = reinterpret_cast<const PackEntry*>(pack->toc_.data());
  for (u32 i = 0; i < header.entry_count; ++i)
    if (entries[i].name_offset >= header.names_size) return nullptr;

  pack->entry_count_ = header.entry_count;
  return pack;
}

PackEntryView PackFile::entry(size_t index) const {
  const PackEntry* entries = reinterpret_cast<const PackEntry*>(toc_.data());
  const PackEntry& e = entries[index];
  return PackEntryView{
      .path = std::string_view(&names_[e.name_offset]),
      .size = e.size,
      .stored_size = e.stored_size,
      .compression = static_cast<PackCompression>(e.compression),
      .crc32 = e.crc32,
  };
}

std::optional<size_t> PackFile::Find(std::string_view normalized_path) const {
  const u64 hash = Fnv1a(normalized_path);
  const PackEntry* entries = reinterpret_cast<const PackEntry*>(toc_.data());
  const size_t count = entry_count();
  const PackEntry* it = std::lower_bound(
      entries, entries + count, hash,
      [](const PackEntry& e, u64 value) { return e.path_hash < value; });
  for (; it != entries + count && it->path_hash == hash; ++it) {
    if (std::string_view(&names_[it->name_offset]) == normalized_path)
      return static_cast<size_t>(it - entries);
  }
  return std::nullopt;
}

std::optional<base::Vector<u8>> PackFile::ReadEntry(size_t index) const {
  const PackEntry* entries = reinterpret_cast<const PackEntry*>(toc_.data());
  const PackEntry& e = entries[index];

  base::Vector<u8> stored(e.stored_size);
  {
    std::scoped_lock lock(io_mutex_);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(e.offset));
    if (e.stored_size > 0 &&
        !file_.read(reinterpret_cast<char*>(stored.data()),
                    static_cast<std::streamsize>(e.stored_size)))
      return std::nullopt;
  }

  base::Vector<u8> out;
  switch (static_cast<PackCompression>(e.compression)) {
    case PackCompression::kStore:
      out = std::move(stored);
      break;
    case PackCompression::kDeflate: {
      out.resize(e.size);
      mz_ulong out_size = static_cast<mz_ulong>(e.size);
      if (mz_uncompress(out.data(), &out_size, stored.data(),
                        static_cast<mz_ulong>(stored.size())) != MZ_OK ||
          out_size != e.size)
        return std::nullopt;
      break;
    }
    default:
      return std::nullopt;
  }

  if (out.size() != e.size) return std::nullopt;
  if (Crc32(out.data(), out.size()) != e.crc32) return std::nullopt;
  return out;
}

// --- write side --------------------------------------------------------------

void PackWriter::Add(std::string_view virtual_path, base::Vector<u8> bytes,
                     PackCompression compression) {
  std::string normalized = NormalizePath(virtual_path);
  for (auto& pending : pending_) {
    if (pending.path == normalized) {
      pending.bytes = std::move(bytes);
      pending.compression = compression;
      return;
    }
  }
  pending_.push_back(Pending{std::move(normalized), std::move(bytes), compression});
}

bool PackWriter::WriteTo(const std::string& file_path) {
  struct Staged {
    const Pending* source;
    u64 hash;
    base::Vector<u8> compressed;  // payload as stored (may alias nothing when raw)
    PackCompression method;
  };

  base::Vector<Staged> staged;
  for (const Pending& pending : pending_) {
    Staged s{&pending, Fnv1a(pending.path), {}, PackCompression::kStore};
    // mz_ulong is 32-bit on some ABIs; oversized payloads are stored raw.
    if (pending.compression == PackCompression::kDeflate && !pending.bytes.empty() &&
        pending.bytes.size() < 0xFFFFFFFFull) {
      mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(pending.bytes.size()));
      base::Vector<u8> compressed(bound);
      mz_ulong compressed_size = bound;
      if (mz_compress2(compressed.data(), &compressed_size, pending.bytes.data(),
                       static_cast<mz_ulong>(pending.bytes.size()), compression_level_) == MZ_OK &&
          compressed_size < pending.bytes.size()) {
        compressed.resize(compressed_size);
        s.compressed = std::move(compressed);
        s.method = PackCompression::kDeflate;
      }
    }
    staged.push_back(std::move(s));
  }

  std::sort(staged.begin(), staged.end(), [](const Staged& a, const Staged& b) {
    if (a.hash != b.hash) return a.hash < b.hash;
    return a.source->path < b.source->path;
  });

  base::Vector<char> names;
  base::Vector<PackEntry> entries;
  for (const Staged& s : staged) {
    PackEntry e{};
    e.path_hash = s.hash;
    e.name_offset = static_cast<u32>(names.size());
    e.size = s.source->bytes.size();
    e.stored_size = s.method == PackCompression::kStore ? s.source->bytes.size()
                                                        : s.compressed.size();
    e.compression = static_cast<u8>(s.method);
    e.crc32 = Crc32(s.source->bytes.data(), s.source->bytes.size());
    for (char c : s.source->path) names.push_back(c);
    names.push_back('\0');
    entries.push_back(e);
  }

  PackHeader header{};
  std::memcpy(header.magic, kMagic, sizeof(kMagic));
  header.version = kVersion;
  header.entry_count = static_cast<u32>(entries.size());
  header.names_offset = sizeof(PackHeader) + entries.size() * sizeof(PackEntry);
  header.names_size = names.size();
  header.data_offset = AlignUp(header.names_offset + header.names_size, kPayloadAlignment);

  u64 cursor = header.data_offset;
  for (PackEntry& e : entries) {
    e.offset = cursor;
    cursor = AlignUp(cursor + e.stored_size, kPayloadAlignment);
  }

  mz_ulong toc_crc = mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const u8*>(entries.data()),
                              entries.size() * sizeof(PackEntry));
  toc_crc = mz_crc32(toc_crc, reinterpret_cast<const u8*>(names.data()), names.size());
  header.toc_crc32 = static_cast<u32>(toc_crc);

  std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  out.write(reinterpret_cast<const char*>(entries.data()),
            static_cast<std::streamsize>(entries.size() * sizeof(PackEntry)));
  out.write(names.data(), static_cast<std::streamsize>(names.size()));

  u64 written = header.names_offset + header.names_size;
  for (size_t i = 0; i < staged.size(); ++i) {
    const Staged& s = staged[i];
    const PackEntry& e = entries[i];
    for (; written < e.offset; ++written) out.put('\0');
    const u8* payload =
        s.method == PackCompression::kStore ? s.source->bytes.data() : s.compressed.data();
    out.write(reinterpret_cast<const char*>(payload), static_cast<std::streamsize>(e.stored_size));
    written += e.stored_size;
  }
  out.flush();
  return static_cast<bool>(out);
}

// --- Vfs provider ------------------------------------------------------------

namespace {

class PackFileProvider final : public FileProvider {
 public:
  explicit PackFileProvider(base::UniquePointer<PackFile> pack) : pack_(std::move(pack)) {}

  bool Contains(std::string_view normalized_path) const override {
    return pack_->Find(normalized_path).has_value();
  }

  std::optional<base::Vector<u8>> Read(std::string_view normalized_path) const override {
    const std::optional<size_t> index = pack_->Find(normalized_path);
    if (!index) return std::nullopt;
    return pack_->ReadEntry(*index);
  }

  std::optional<u64> Size(std::string_view normalized_path) const override {
    const std::optional<size_t> index = pack_->Find(normalized_path);
    if (!index) return std::nullopt;
    return pack_->entry(*index).size;
  }

  void Enumerate(const std::function<void(std::string_view)>& fn) const override {
    for (size_t i = 0; i < pack_->entry_count(); ++i) fn(pack_->entry(i).path);
  }

  std::string name() const override { return pack_->path(); }

 private:
  base::UniquePointer<PackFile> pack_;
};

}  // namespace

base::UniquePointer<FileProvider> MakePackFileProvider(std::string archive_path) {
  base::UniquePointer<PackFile> pack = PackFile::Open(std::move(archive_path));
  if (!pack) return nullptr;
  return base::MakeUnique<PackFileProvider>(std::move(pack));
}

}  // namespace rx::asset
