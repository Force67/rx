#include "terrain/terrain.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rx::terrain {
namespace {

constexpr u8 kMagic[8] = {'R', 'X', 'T', 'E', 'R', 'R', 'A', 'I'};
constexpr u32 kVersion = 1;
constexpr u32 kMaximumTileQuads = 4096;
constexpr u32 kMaximumLayers = 4;
constexpr u32 kMaximumNameBytes = 4096;
constexpr u32 kMaximumTiles = 1'000'000;
constexpr u64 kMaximumTotalSamples = 64'000'000;
constexpr u64 kMaximumFileBytes = 2ull * 1024 * 1024 * 1024;

void SetError(std::string *error, std::string message) {
  if (error)
    *error = std::move(message);
}

void AppendU8(base::Vector<u8> *bytes, u8 value) { bytes->push_back(value); }

void AppendU16(base::Vector<u8> *bytes, u16 value) {
  AppendU8(bytes, static_cast<u8>(value));
  AppendU8(bytes, static_cast<u8>(value >> 8));
}

void AppendU32(base::Vector<u8> *bytes, u32 value) {
  for (u32 shift = 0; shift < 32; shift += 8)
    AppendU8(bytes, static_cast<u8>(value >> shift));
}

void AppendU64(base::Vector<u8> *bytes, u64 value) {
  for (u32 shift = 0; shift < 64; shift += 8)
    AppendU8(bytes, static_cast<u8>(value >> shift));
}

void AppendF32(base::Vector<u8> *bytes, f32 value) {
  AppendU32(bytes, std::bit_cast<u32>(value));
}

void AppendString(base::Vector<u8> *bytes, std::string_view value) {
  AppendU32(bytes, static_cast<u32>(value.size()));
  bytes->insert(bytes->end(), value.begin(), value.end());
}

u64 Checksum(std::span<const u8> bytes) {
  u64 hash = 0xcbf29ce484222325ull;
  for (u8 byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ull;
  }
  return hash;
}

bool KeyLess(TerrainTileKey a, TerrainTileKey b) {
  return a.x != b.x ? a.x < b.x : a.z < b.z;
}

bool IsNormalized(TerrainWeights value, u32 layer_count) {
  u32 total = 0;
  for (u32 i = 0; i < layer_count; ++i)
    total += value.rgba[i];
  for (u32 i = layer_count; i < 4; ++i) {
    if (value.rgba[i] != 0)
      return false;
  }
  return total == 255;
}

bool GetSampleCount(u32 quads, u32 *count) {
  if (quads == 0 || quads > kMaximumTileQuads)
    return false;
  const u64 side = static_cast<u64>(quads) + 1;
  const u64 samples = side * side;
  if (samples > std::numeric_limits<u32>::max())
    return false;
  *count = static_cast<u32>(samples);
  return true;
}

bool ValidateTerrain(const Terrain &terrain, u32 *sample_count,
                     std::string *error) {
  const TerrainDesc &desc = terrain.desc();
  if (!desc.id || !std::isfinite(desc.origin.x) || !std::isfinite(desc.origin.y) ||
      !std::isfinite(desc.origin.z) || !std::isfinite(desc.sample_spacing) ||
      desc.sample_spacing <= 0 ||
      !GetSampleCount(desc.tile_quads, sample_count)) {
    SetError(error, "terrain has invalid dimensions or non-finite metadata");
    return false;
  }
  const f32 tile_width = desc.tile_quads * desc.sample_spacing;
  if (!std::isfinite(tile_width)) {
    SetError(error, "terrain tile width is not finite");
    return false;
  }
  if (desc.layers.empty() || desc.layers.size() > kMaximumLayers) {
    SetError(error, "terrain palette must contain one to four layers");
    return false;
  }
  for (const TerrainLayer &layer : desc.layers) {
    if (layer.name.size() > kMaximumNameBytes) {
      SetError(error, "terrain layer name is too long");
      return false;
    }
  }
  if (terrain.tiles().size() > kMaximumTiles ||
      static_cast<u64>(terrain.tiles().size()) * *sample_count >
          kMaximumTotalSamples) {
    SetError(error, "terrain tile or sample count exceeds format limits");
    return false;
  }

  TerrainTileKey previous;
  bool has_previous = false;
  for (const TerrainTile &tile : terrain.tiles()) {
    if ((has_previous && !KeyLess(previous, tile.key)) ||
        tile.heights.size() != *sample_count ||
        tile.weights.size() != *sample_count) {
      SetError(error, "terrain tiles are unsorted or have invalid dimensions");
      return false;
    }
    previous = tile.key;
    has_previous = true;
    for (u32 i = 0; i < *sample_count; ++i) {
      if (!std::isfinite(tile.heights[i]) ||
          !IsNormalized(tile.weights[i],
                        static_cast<u32>(desc.layers.size()))) {
        SetError(error, "terrain tile contains invalid height or weight data");
        return false;
      }
    }
  }
  return true;
}

class Reader {
public:
  explicit Reader(std::span<const u8> bytes) : bytes_(bytes) {}

  bool ReadU8(u8 *value) {
    if (remaining() < 1)
      return false;
    *value = bytes_[offset_++];
    return true;
  }

  bool ReadU16(u16 *value) {
    if (remaining() < 2)
      return false;
    *value = static_cast<u16>(bytes_[offset_]) |
             static_cast<u16>(bytes_[offset_ + 1]) << 8;
    offset_ += 2;
    return true;
  }

  bool ReadU32(u32 *value) {
    if (remaining() < 4)
      return false;
    *value = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
      *value |= static_cast<u32>(bytes_[offset_++]) << shift;
    }
    return true;
  }

  bool ReadU64(u64 *value) {
    if (remaining() < 8)
      return false;
    *value = 0;
    for (u32 shift = 0; shift < 64; shift += 8) {
      *value |= static_cast<u64>(bytes_[offset_++]) << shift;
    }
    return true;
  }

  bool ReadI32(i32 *value) {
    u32 bits = 0;
    if (!ReadU32(&bits))
      return false;
    *value = std::bit_cast<i32>(bits);
    return true;
  }

  bool ReadF32(f32 *value) {
    u32 bits = 0;
    if (!ReadU32(&bits))
      return false;
    *value = std::bit_cast<f32>(bits);
    return true;
  }

  bool ReadString(u32 maximum_size, std::string *value) {
    u32 size = 0;
    if (!ReadU32(&size) || size > maximum_size || remaining() < size)
      return false;
    value->assign(reinterpret_cast<const char *>(bytes_.data() + offset_),
                  size);
    offset_ += size;
    return true;
  }

  size_t remaining() const { return bytes_.size() - offset_; }

private:
  std::span<const u8> bytes_;
  size_t offset_ = 0;
};

bool ReadFile(const std::string &file_path, base::Vector<u8> *bytes,
              std::string *error) {
  std::ifstream input(file_path, std::ios::binary | std::ios::ate);
  if (!input) {
    SetError(error, "cannot open terrain file for reading");
    return false;
  }
  const std::streamoff size = input.tellg();
  if (size < 0 || static_cast<u64>(size) > kMaximumFileBytes) {
    SetError(error, "terrain file size is invalid");
    return false;
  }
  bytes->resize(static_cast<size_t>(size));
  input.seekg(0);
  if (!bytes->empty()) {
    input.read(reinterpret_cast<char *>(bytes->data()),
               static_cast<std::streamsize>(bytes->size()));
  }
  if (!input) {
    SetError(error, "terrain file is truncated");
    return false;
  }
  return true;
}

bool ReplaceFile(const std::filesystem::path& temporary,
                 const std::filesystem::path& target,
                 std::error_code* error) {
  std::filesystem::rename(temporary, target, *error);
#if defined(_WIN32)
  if (*error && MoveFileExW(temporary.c_str(), target.c_str(),
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    error->clear();
  }
#endif
  return !*error;
}

} // namespace

bool SaveTerrain(const Terrain &terrain, const std::string &file_path,
                 std::string *error) {
  if (error)
    error->clear();
  u32 sample_count = 0;
  if (!ValidateTerrain(terrain, &sample_count, error))
    return false;

  base::Vector<u8> bytes;
  bytes.insert(bytes.end(), std::begin(kMagic), std::end(kMagic));
  AppendU32(&bytes, kVersion);
  const TerrainDesc &desc = terrain.desc();
  AppendU64(&bytes, desc.id.hash);
  AppendF32(&bytes, desc.origin.x);
  AppendF32(&bytes, desc.origin.y);
  AppendF32(&bytes, desc.origin.z);
  AppendU32(&bytes, desc.tile_quads);
  AppendF32(&bytes, desc.sample_spacing);
  AppendU32(&bytes, static_cast<u32>(desc.layers.size()));
  AppendU32(&bytes, static_cast<u32>(terrain.tiles().size()));
  for (const TerrainLayer &layer : desc.layers) {
    AppendString(&bytes, layer.name);
    AppendU64(&bytes, layer.albedo.hash);
    AppendU64(&bytes, layer.normal.hash);
    for (u8 channel : layer.debug_rgba)
      AppendU8(&bytes, channel);
  }

  for (const TerrainTile &tile : terrain.tiles()) {
    AppendU32(&bytes, std::bit_cast<u32>(tile.key.x));
    AppendU32(&bytes, std::bit_cast<u32>(tile.key.z));
    AppendU64(&bytes, tile.revision);
    const auto [minimum_it, maximum_it] =
        std::minmax_element(tile.heights.begin(), tile.heights.end());
    const f32 minimum = *minimum_it;
    const f32 range = *maximum_it - minimum;
    if (!std::isfinite(range)) {
      SetError(error, "terrain tile height range is not finite");
      return false;
    }
    AppendF32(&bytes, minimum);
    AppendF32(&bytes, range);
    AppendU32(&bytes, sample_count);
    AppendU32(&bytes, sample_count * 4);
    for (f32 height : tile.heights) {
      u16 quantized = 0;
      if (range > 0) {
        const double normalized = (static_cast<double>(height) - minimum) /
                                  static_cast<double>(range);
        quantized = static_cast<u16>(
            std::clamp(std::llround(normalized * 65535.0), 0ll, 65535ll));
      }
      AppendU16(&bytes, quantized);
    }
    for (TerrainWeights weights : tile.weights) {
      for (u8 channel : weights.rgba)
        AppendU8(&bytes, channel);
    }
  }
  AppendU64(&bytes, Checksum(bytes));

  const std::filesystem::path target(file_path);
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  std::error_code filesystem_error;
  std::filesystem::remove(temporary, filesystem_error);
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) {
    SetError(error, "cannot open terrain temporary file for writing");
    return false;
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  const bool wrote = static_cast<bool>(output);
  output.close();
  if (!wrote) {
    std::filesystem::remove(temporary, filesystem_error);
    SetError(error, "failed writing terrain temporary file");
    return false;
  }

  if (!ReplaceFile(temporary, target, &filesystem_error)) {
    std::filesystem::remove(temporary, filesystem_error);
    SetError(error, "failed renaming terrain temporary file");
    return false;
  }
  return true;
}

bool LoadTerrain(const std::string &file_path, Terrain *terrain,
                 std::string *error) {
  if (error)
    error->clear();
  if (!terrain) {
    SetError(error, "terrain output is null");
    return false;
  }
  base::Vector<u8> bytes;
  if (!ReadFile(file_path, &bytes, error))
    return false;
  if (bytes.size() < sizeof(kMagic) + sizeof(u32) + sizeof(u64)) {
    SetError(error, "terrain file is truncated");
    return false;
  }
  const size_t payload_size = bytes.size() - sizeof(u64);
  u64 stored_checksum = 0;
  for (u32 shift = 0; shift < 64; shift += 8) {
    stored_checksum |= static_cast<u64>(bytes[payload_size + shift / 8])
                       << shift;
  }
  if (stored_checksum !=
      Checksum(std::span<const u8>(bytes.data(), payload_size))) {
    SetError(error, "terrain file checksum mismatch");
    return false;
  }

  Reader reader(std::span<const u8>(bytes.data(), payload_size));
  for (u8 expected : kMagic) {
    u8 actual = 0;
    if (!reader.ReadU8(&actual) || actual != expected) {
      SetError(error, "not an rxterrain file");
      return false;
    }
  }
  u32 version = 0;
  if (!reader.ReadU32(&version) || version != kVersion) {
    SetError(error, "unsupported rxterrain version");
    return false;
  }

  TerrainDesc desc;
  u32 layer_count = 0;
  u32 tile_count = 0;
  if (!reader.ReadU64(&desc.id.hash) || !reader.ReadF32(&desc.origin.x) ||
      !reader.ReadF32(&desc.origin.y) || !reader.ReadF32(&desc.origin.z) ||
      !reader.ReadU32(&desc.tile_quads) ||
      !reader.ReadF32(&desc.sample_spacing) || !reader.ReadU32(&layer_count) ||
      !reader.ReadU32(&tile_count)) {
    SetError(error, "terrain header is truncated");
    return false;
  }
  u32 sample_count = 0;
  if (!desc.id || !std::isfinite(desc.origin.x) ||
      !std::isfinite(desc.origin.y) ||
      !std::isfinite(desc.origin.z) || !std::isfinite(desc.sample_spacing) ||
      desc.sample_spacing <= 0 ||
      !GetSampleCount(desc.tile_quads, &sample_count) ||
      !std::isfinite(desc.tile_quads * desc.sample_spacing) ||
      layer_count == 0 || layer_count > kMaximumLayers ||
      tile_count > kMaximumTiles ||
      static_cast<u64>(tile_count) * sample_count > kMaximumTotalSamples) {
    SetError(error, "terrain header has invalid dimensions or counts");
    return false;
  }

  desc.layers.resize(layer_count);
  for (TerrainLayer &layer : desc.layers) {
    if (!reader.ReadString(kMaximumNameBytes, &layer.name) ||
        !reader.ReadU64(&layer.albedo.hash) ||
        !reader.ReadU64(&layer.normal.hash)) {
      SetError(error, "terrain palette is truncated or invalid");
      return false;
    }
    for (u8 &channel : layer.debug_rgba) {
      if (!reader.ReadU8(&channel)) {
        SetError(error, "terrain palette is truncated");
        return false;
      }
    }
  }

  Terrain decoded(std::move(desc));
  base::Vector<u64> revisions;
  revisions.reserve(tile_count);
  TerrainTileKey previous;
  bool has_previous = false;
  for (u32 tile_index = 0; tile_index < tile_count; ++tile_index) {
    TerrainTileKey key;
    u64 revision = 0;
    f32 minimum = 0;
    f32 range = 0;
    u32 stored_samples = 0;
    u32 stored_weight_bytes = 0;
    if (!reader.ReadI32(&key.x) || !reader.ReadI32(&key.z) ||
        !reader.ReadU64(&revision) || !reader.ReadF32(&minimum) ||
        !reader.ReadF32(&range) || !reader.ReadU32(&stored_samples) ||
        !reader.ReadU32(&stored_weight_bytes)) {
      SetError(error, "terrain tile header is truncated");
      return false;
    }
    if ((has_previous && !KeyLess(previous, key)) || !std::isfinite(minimum) ||
        !std::isfinite(range) || range < 0 || !std::isfinite(minimum + range) ||
        stored_samples != sample_count ||
        stored_weight_bytes != sample_count * 4 ||
        reader.remaining() < static_cast<size_t>(sample_count) * 6) {
      SetError(error, "terrain tile dimensions or height range are invalid");
      return false;
    }
    previous = key;
    has_previous = true;

    base::Vector<f32> heights(sample_count);
    for (f32 &height : heights) {
      u16 quantized = 0;
      if (!reader.ReadU16(&quantized) || (range == 0 && quantized != 0)) {
        SetError(error, "terrain height data is truncated or invalid");
        return false;
      }
      height = minimum + range * (static_cast<f32>(quantized) / 65535.0f);
      if (!std::isfinite(height)) {
        SetError(error, "terrain height data is not finite");
        return false;
      }
    }
    base::Vector<TerrainWeights> weights(sample_count);
    for (TerrainWeights &value : weights) {
      for (u8 &channel : value.rgba) {
        if (!reader.ReadU8(&channel)) {
          SetError(error, "terrain weight data is truncated");
          return false;
        }
      }
      if (!IsNormalized(value, layer_count)) {
        SetError(error, "terrain weight data is not normalized");
        return false;
      }
    }
    if (!decoded.AddOrReplaceTile(key, heights, weights)) {
      SetError(error, "terrain tile could not be reconstructed");
      return false;
    }
    revisions.push_back(revision);
  }
  if (reader.remaining() != 0) {
    SetError(error, "terrain file has trailing payload data");
    return false;
  }
  for (u32 i = 0; i < tile_count; ++i)
    decoded.tiles_[i].revision = revisions[i];
  *terrain = std::move(decoded);
  return true;
}

} // namespace rx::terrain
