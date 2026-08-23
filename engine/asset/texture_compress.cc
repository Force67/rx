#include "asset/texture_compress.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "asset/asset_id.h"
#include "asset/bc_encode.h"
#include "core/log.h"

namespace rx::asset {
namespace fs = std::filesystem;
namespace {

// Bumped whenever the encoder or the mip filter changes what it produces.
// It rides in the cache key, so an old entry is never read back by a newer
// encoder. Nothing prunes them, though: every codec bump, format change and
// edited source strands its old entry, at up to 21 MB for a 4K texture, and
// the directory only ever grows. Deleting it is safe and costs one re-encode.
constexpr u32 kCodecVersion = 1;
constexpr char kCacheMagic[4] = {'R', 'X', 'T', 'C'};

std::atomic<bool> g_supported{false};
std::atomic<bool> g_compress_normals{false};

std::mutex g_stats_mutex;
TextureCompressionStats g_stats;

// --- colour space -----------------------------------------------------------

const f32* SrgbToLinearTable() {
  static const auto* table = [] {
    auto* values = new f32[256];
    for (u32 i = 0; i < 256; ++i) {
      const f32 c = static_cast<f32>(i) / 255.0f;
      values[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return values;
  }();
  return table;
}

u8 LinearToSrgb(f32 v) {
  v = std::clamp(v, 0.0f, 1.0f);
  const f32 s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
  return static_cast<u8>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
}

// --- mip chain --------------------------------------------------------------

u32 FullMipChainLength(u32 width, u32 height) {
  u32 levels = 1;
  while (width > 1 || height > 1) {
    width = std::max(1u, width / 2);
    height = std::max(1u, height / 2);
    ++levels;
  }
  return levels;
}

// Bilinear resample with the same texel mapping vkCmdBlitImage uses, because
// that is what the rgba8 path this replaces was doing on the gpu: for an exact
// halving it collapses to a 2x2 box, and it stays right on the odd sizes a
// non-power-of-two chain hits. Colour is filtered in linear light; averaging
// sRGB-encoded values darkens every mip, which is visible as terrain that gets
// muddier with distance.
void Downsample(const u8* src, u32 sw, u32 sh, u8* dst, u32 dw, u32 dh, bool srgb) {
  const f32* to_linear = SrgbToLinearTable();
  const f32 scale_x = static_cast<f32>(sw) / static_cast<f32>(dw);
  const f32 scale_y = static_cast<f32>(sh) / static_cast<f32>(dh);
  for (u32 y = 0; y < dh; ++y) {
    const f32 fy = (static_cast<f32>(y) + 0.5f) * scale_y - 0.5f;
    const i32 y0 = static_cast<i32>(std::floor(fy));
    const f32 wy = fy - static_cast<f32>(y0);
    const u32 ya = static_cast<u32>(std::clamp(y0, 0, static_cast<i32>(sh) - 1));
    const u32 yb = static_cast<u32>(std::clamp(y0 + 1, 0, static_cast<i32>(sh) - 1));
    for (u32 x = 0; x < dw; ++x) {
      const f32 fx = (static_cast<f32>(x) + 0.5f) * scale_x - 0.5f;
      const i32 x0 = static_cast<i32>(std::floor(fx));
      const f32 wx = fx - static_cast<f32>(x0);
      const u32 xa = static_cast<u32>(std::clamp(x0, 0, static_cast<i32>(sw) - 1));
      const u32 xb = static_cast<u32>(std::clamp(x0 + 1, 0, static_cast<i32>(sw) - 1));
      const u8* p00 = src + (static_cast<size_t>(ya) * sw + xa) * 4;
      const u8* p10 = src + (static_cast<size_t>(ya) * sw + xb) * 4;
      const u8* p01 = src + (static_cast<size_t>(yb) * sw + xa) * 4;
      const u8* p11 = src + (static_cast<size_t>(yb) * sw + xb) * 4;
      const f32 w00 = (1.0f - wx) * (1.0f - wy);
      const f32 w10 = wx * (1.0f - wy);
      const f32 w01 = (1.0f - wx) * wy;
      const f32 w11 = wx * wy;
      u8* out = dst + (static_cast<size_t>(y) * dw + x) * 4;
      for (u32 c = 0; c < 4; ++c) {
        if (srgb && c < 3) {
          const f32 v = to_linear[p00[c]] * w00 + to_linear[p10[c]] * w10 +
                        to_linear[p01[c]] * w01 + to_linear[p11[c]] * w11;
          out[c] = LinearToSrgb(v);
        } else {
          const f32 v = static_cast<f32>(p00[c]) * w00 + static_cast<f32>(p10[c]) * w10 +
                        static_cast<f32>(p01[c]) * w01 + static_cast<f32>(p11[c]) * w11;
          out[c] = static_cast<u8>(std::clamp(v + 0.5f, 0.0f, 255.0f));
        }
      }
    }
  }
}

// --- block layout -----------------------------------------------------------

u32 BlockBytes(TextureFormat format) {
  switch (format) {
    case TextureFormat::kBc1:
    case TextureFormat::kBc4:
      return 8;
    case TextureFormat::kBc2:
    case TextureFormat::kBc3:
    case TextureFormat::kBc5:
    case TextureFormat::kBc7:
      return 16;
    default:
      return 0;
  }
}

u64 SurfaceBytes(TextureFormat format, u32 width, u32 height) {
  const u64 blocks_x = (static_cast<u64>(width) + 3) / 4;
  const u64 blocks_y = (static_cast<u64>(height) + 3) / 4;
  return blocks_x * blocks_y * BlockBytes(format);
}

void EncodeBlock(TextureFormat format, const u8* rgba, u8* out) {
  switch (format) {
    case TextureFormat::kBc1:
      EncodeBc1Block(rgba, out);
      return;
    case TextureFormat::kBc3:
      EncodeBc3Block(rgba, out);
      return;
    case TextureFormat::kBc5:
      EncodeBc5Block(rgba, out);
      return;
    case TextureFormat::kBc7:
      EncodeBc7Block(rgba, out);
      return;
    default:
      // BlockBytes answers for kBc2 and kBc4, so a format reaching here passes
      // the size guard in CompressTexture and would then be written out of an
      // uninitialised buffer, which is a different texture on every run.
      // FormatForRole emits neither today; this says so once and leaves a
      // block that is at least deterministic.
      RX_ERROR("EncodeBlock: no encoder for texture format {}, writing an empty block",
               static_cast<int>(format));
      std::memset(out, 0, BlockBytes(format));
      return;
  }
}

// Encodes one mip. Block rows are independent, so the level is split across
// hardware threads; loading a scene is serial anyway, so there is nothing else
// wanting the cores at this point.
void EncodeSurface(const u8* rgba, u32 width, u32 height, TextureFormat format, u8* out) {
  const u32 blocks_x = (width + 3) / 4;
  const u32 blocks_y = (height + 3) / 4;
  const u32 block_bytes = BlockBytes(format);

  auto encode_rows = [&](u32 first_row, u32 last_row) {
    u8 block[kBlockRgbaBytes];
    for (u32 by = first_row; by < last_row; ++by) {
      for (u32 bx = 0; bx < blocks_x; ++bx) {
        for (u32 ty = 0; ty < 4; ++ty) {
          // Edges that are not a multiple of four replicate the last real
          // texel into the padding. Leaving it zero would pull the endpoint
          // fit toward black on every edge block.
          const u32 sy = std::min(by * 4 + ty, height - 1);
          for (u32 tx = 0; tx < 4; ++tx) {
            const u32 sx = std::min(bx * 4 + tx, width - 1);
            std::memcpy(block + (ty * 4 + tx) * 4, rgba + (static_cast<size_t>(sy) * width + sx) * 4,
                        4);
          }
        }
        EncodeBlock(format, block,
                    out + (static_cast<size_t>(by) * blocks_x + bx) * block_bytes);
      }
    }
  };

  unsigned threads = std::thread::hardware_concurrency();
  if (threads == 0) threads = 1;
  threads = std::min<unsigned>(threads, blocks_y);
  if (threads <= 1) {
    encode_rows(0, blocks_y);
    return;
  }
  std::vector<std::thread> workers;
  workers.reserve(threads - 1);
  const u32 per_thread = (blocks_y + threads - 1) / threads;
  for (unsigned t = 1; t < threads; ++t) {
    const u32 first = std::min(blocks_y, t * per_thread);
    const u32 last = std::min(blocks_y, first + per_thread);
    if (first >= last) break;
    workers.emplace_back([&, first, last] { encode_rows(first, last); });
  }
  encode_rows(0, std::min(blocks_y, per_thread));
  for (std::thread& worker : workers) worker.join();
}

// --- format choice ----------------------------------------------------------

bool HasAlpha(const Texture& texture) {
  const size_t texels = static_cast<size_t>(texture.width) * texture.height;
  for (size_t i = 0; i < texels; ++i) {
    if (texture.data[i * 4 + 3] != 255) return true;
  }
  return false;
}

TextureFormat FormatForRole(const Texture& texture, TextureRole role) {
  switch (role) {
    case TextureRole::kColor:
      return HasAlpha(texture) ? TextureFormat::kBc3 : TextureFormat::kBc7;
    case TextureRole::kNormalTangent:
      // Not BC7 as a consolation prize when normals are off: BC7 mode 6 runs
      // one index ramp across rgb, and a normal map's three channels move
      // independently, so it comes out worse than BC5 AND worse than leaving
      // the map alone. Skipping is the honest answer.
      return g_compress_normals.load() ? TextureFormat::kBc5 : TextureFormat::kUnknown;
    case TextureRole::kData:
      return TextureFormat::kBc7;
  }
  return TextureFormat::kUnknown;
}

// --- disk cache -------------------------------------------------------------

std::string CacheRoot() {
  if (const char* override_dir = std::getenv("RX_TEXCACHE_DIR")) return override_dir;
  if (const char* xdg = std::getenv("XDG_CACHE_HOME")) return (fs::path(xdg) / "rx/texcache").string();
#if defined(_WIN32)
  if (const char* local = std::getenv("LOCALAPPDATA"))
    return (fs::path(local) / "rx/texcache").string();
#else
  if (const char* home = std::getenv("HOME")) return (fs::path(home) / ".cache/rx/texcache").string();
#endif
  return (fs::temp_directory_path() / "rx/texcache").string();
}

// Keyed on the SOURCE PIXELS, not on the file path: the same image embedded in
// forty glb files, or reached through two different relative paths, is one
// cache entry and one encode. The cost is hashing the decoded mip 0 on every
// load, which is a couple of milliseconds against an encode measured in tens.
std::string CacheKey(const Texture& texture, TextureFormat format) {
  const u64 content =
      MakeAssetId(std::string_view(reinterpret_cast<const char*>(texture.data.data()),
                                   texture.data.size()))
          .hash;
  char shape[96];
  std::snprintf(shape, sizeof(shape), "%016llx:%u:%u:%u:%u:%u",
                static_cast<unsigned long long>(content), texture.width, texture.height,
                static_cast<u32>(format), texture.is_srgb ? 1u : 0u, kCodecVersion);
  char key[17];
  std::snprintf(key, sizeof(key), "%016llx",
                static_cast<unsigned long long>(MakeAssetId(shape).hash));
  return key;
}

struct CacheHeader {
  char magic[4];
  u32 version;
  u32 format;
  u32 width;
  u32 height;
  u32 mip_count;
  u32 srgb;
  u64 data_bytes;
};

bool ReadCache(const fs::path& path, Texture* texture, TextureFormat format) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  CacheHeader header{};
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  if (!file) return false;
  if (std::memcmp(header.magic, kCacheMagic, 4) != 0 || header.version != kCodecVersion) return false;
  // The key already covers all of this; a mismatch means a hash collision or a
  // hand-edited cache, and trusting it would upload a texture of the wrong
  // shape. Cheaper to re-encode than to debug that.
  if (header.format != static_cast<u32>(format) || header.width != texture->width ||
      header.height != texture->height || header.srgb != (texture->is_srgb ? 1u : 0u)) {
    return false;
  }
  if (header.data_bytes == 0 || header.data_bytes > (1ull << 32)) return false;
  if (header.mip_count != FullMipChainLength(texture->width, texture->height)) return false;
  base::Vector<u8> data(static_cast<size_t>(header.data_bytes));
  file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!file) return false;
  texture->format = format;
  texture->mip_count = header.mip_count;
  texture->data = std::move(data);
  return true;
}

void WriteCache(const fs::path& path, const Texture& texture) {
  std::error_code error;
  fs::create_directories(path.parent_path(), error);
  // Write beside the target and rename: two processes loading the same scene,
  // or one killed mid-write, must not leave a truncated file that the next run
  // reads back as a valid texture.
  // The pid is in the name because the temp path is otherwise derived from the
  // cache key alone, so two processes compressing the same texture opened the
  // same file, one truncating it while the other was mid-write. It survived
  // only because both were writing identical bytes.
#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = getpid();
#endif
  const fs::path temp = fs::path(path).concat("." + std::to_string(pid) + ".tmp");
  {
    std::ofstream file(temp, std::ios::binary | std::ios::trunc);
    if (!file) return;
    CacheHeader header{};
    std::memcpy(header.magic, kCacheMagic, 4);
    header.version = kCodecVersion;
    header.format = static_cast<u32>(texture.format);
    header.width = texture.width;
    header.height = texture.height;
    header.mip_count = texture.mip_count;
    header.srgb = texture.is_srgb ? 1u : 0u;
    header.data_bytes = texture.data.size();
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.write(reinterpret_cast<const char*>(texture.data.data()),
               static_cast<std::streamsize>(texture.data.size()));
    if (!file) {
      file.close();
      fs::remove(temp, error);
      return;
    }
  }
  fs::rename(temp, path, error);
  if (error) fs::remove(temp, error);
}

}  // namespace

void SetTextureCompression(TextureCompressionOptions options) {
  g_supported.store(options.supported);
  g_compress_normals.store(options.normals);
}

TextureCompressionOptions TextureCompressionSettings() {
  return {g_supported.load(), g_compress_normals.load()};
}

bool CompressTexture(Texture* texture, TextureRole role, std::string_view identity) {
  auto skip = [] {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    ++g_stats.skipped;
    return false;
  };
  if (!texture) return false;
  // Counted, not returned bare. `skipped` is the only signal that separates a
  // build where compression did nothing because it was switched off or the
  // texture was already compressed, from one where it silently did nothing at
  // all, and the header promises it counts both of these.
  if (!g_supported.load()) return skip();
  if (texture->format != TextureFormat::kRgba8) return skip();
  if (texture->array_layers != 1 || texture->is_cubemap) return skip();
  // Under 4x4 there is no whole block to fit, and a 4-multiple edge keeps every
  // level of the chain on whole blocks except the 2x2 and 1x1 tail, which the
  // copy handles as a partial block.
  if (texture->width < 4 || texture->height < 4) return skip();
  if ((texture->width % 4) != 0 || (texture->height % 4) != 0) {
    RX_INFO("texture '{}' is {}x{}, not a multiple of 4; left uncompressed",
            identity.empty() ? "<generated>" : identity, texture->width, texture->height);
    return skip();
  }
  const size_t expected = static_cast<size_t>(texture->width) * texture->height * 4;
  if (texture->data.size() < expected) {
    // The one case in here that is a fault rather than a setting: the texture
    // says one size and carries less. Loudly, or it reads as "left
    // uncompressed" alongside the deliberate skips above it.
    RX_WARN("texture '{}' says {}x{} and carries {} bytes of the {} that needs; not compressed",
            identity.empty() ? "<generated>" : identity, texture->width, texture->height,
            texture->data.size(), expected);
    return skip();
  }

  const TextureFormat format = FormatForRole(*texture, role);
  if (BlockBytes(format) == 0) return skip();

  const std::string key = CacheKey(*texture, format);
  const fs::path cache_path = fs::path(CacheRoot()) / (key + ".rxtc");
  const u64 source_bytes = texture->data.size();
  if (!identity.empty() && ReadCache(cache_path, texture, format)) {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    ++g_stats.compressed;
    ++g_stats.cache_hits;
    g_stats.source_bytes += source_bytes;
    g_stats.compressed_bytes += texture->data.size();
    return true;
  }

  const auto started = std::chrono::steady_clock::now();
  const u32 mip_count = FullMipChainLength(texture->width, texture->height);
  u64 total = 0;
  {
    u32 w = texture->width;
    u32 h = texture->height;
    for (u32 mip = 0; mip < mip_count; ++mip) {
      total += SurfaceBytes(format, w, h);
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
    }
  }
  base::Vector<u8> out(static_cast<size_t>(total));

  // Colour filters in linear light; normal and data maps are already linear,
  // and a normal map is deliberately NOT renormalized per level - letting the
  // averaged xy shorten is what makes a minified normal map flatten instead of
  // sparkle.
  const bool filter_srgb = texture->is_srgb;
  std::vector<u8> level(texture->data.data(), texture->data.data() + expected);
  std::vector<u8> next;
  u32 w = texture->width;
  u32 h = texture->height;
  u64 offset = 0;
  for (u32 mip = 0; mip < mip_count; ++mip) {
    EncodeSurface(level.data(), w, h, format, out.data() + offset);
    offset += SurfaceBytes(format, w, h);
    if (mip + 1 == mip_count) break;
    const u32 nw = std::max(1u, w / 2);
    const u32 nh = std::max(1u, h / 2);
    next.resize(static_cast<size_t>(nw) * nh * 4);
    Downsample(level.data(), w, h, next.data(), nw, nh, filter_srgb);
    level.swap(next);
    w = nw;
    h = nh;
  }

  texture->format = format;
  texture->mip_count = mip_count;
  texture->data = std::move(out);
  const f64 seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
  if (!identity.empty()) WriteCache(cache_path, *texture);
  {
    std::lock_guard<std::mutex> lock(g_stats_mutex);
    ++g_stats.compressed;
    g_stats.source_bytes += source_bytes;
    g_stats.compressed_bytes += texture->data.size();
    g_stats.encode_seconds += seconds;
  }
  return true;
}

TextureCompressionStats CompressionTotals() {
  std::lock_guard<std::mutex> lock(g_stats_mutex);
  return g_stats;
}

}  // namespace rx::asset
