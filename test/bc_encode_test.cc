// The BC encoders, checked against decoders written straight from the format
// definitions rather than against the encoder's own idea of what it wrote. A
// block encoder that packs its fields in the wrong order still round-trips
// through a matching decoder, so the decoders here are deliberately a second
// implementation.
//
// The thresholds are quality floors, not exact values: they are set well below
// what the encoder currently reaches, so a real regression trips them and a
// harmless change in the fit does not.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "asset/bc_encode.h"
#include "asset/texture_compress.h"

namespace {

using namespace rx;

int failures = 0;

void Check(bool condition, const char* message) {
  if (condition) return;
  std::fprintf(stderr, "bc_encode_test: FAIL: %s\n", message);
  ++failures;
}

// --- reference decoders -----------------------------------------------------

u32 Bits(const u8* block, u32 offset, u32 count) {
  u32 value = 0;
  for (u32 i = 0; i < count; ++i) {
    const u32 b = offset + i;
    value |= static_cast<u32>((block[b >> 3] >> (b & 7u)) & 1u) << i;
  }
  return value;
}

void DecodeBc1(const u8* block, u8* rgba) {
  const u32 c0 = block[0] | (block[1] << 8);
  const u32 c1 = block[2] | (block[3] << 8);
  u8 palette[4][4];
  auto expand = [](u32 v, u8* out) {
    const u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    out[0] = static_cast<u8>((r << 3) | (r >> 2));
    out[1] = static_cast<u8>((g << 2) | (g >> 4));
    out[2] = static_cast<u8>((b << 3) | (b >> 2));
    out[3] = 255;
  };
  expand(c0, palette[0]);
  expand(c1, palette[1]);
  for (u32 c = 0; c < 3; ++c) {
    if (c0 > c1) {
      palette[2][c] = static_cast<u8>((2 * palette[0][c] + palette[1][c]) / 3);
      palette[3][c] = static_cast<u8>((palette[0][c] + 2 * palette[1][c]) / 3);
    } else {
      palette[2][c] = static_cast<u8>((palette[0][c] + palette[1][c]) / 2);
      palette[3][c] = 0;
    }
  }
  palette[2][3] = 255;
  palette[3][3] = c0 > c1 ? 255 : 0;
  const u32 indices = block[4] | (block[5] << 8) | (block[6] << 16) |
                      (static_cast<u32>(block[7]) << 24);
  for (u32 t = 0; t < 16; ++t) {
    std::memcpy(rgba + t * 4, palette[(indices >> (t * 2)) & 3u], 4);
  }
}

void DecodeBc4(const u8* block, u8* out16) {
  const u32 r0 = block[0], r1 = block[1];
  u32 palette[8];
  palette[0] = r0;
  palette[1] = r1;
  if (r0 > r1) {
    for (u32 i = 2; i < 8; ++i) palette[i] = ((8 - i) * r0 + (i - 1) * r1) / 7;
  } else {
    for (u32 i = 2; i < 6; ++i) palette[i] = ((6 - i) * r0 + (i - 1) * r1) / 5;
    palette[6] = 0;
    palette[7] = 255;
  }
  rx::u64 bits = 0;
  for (u32 i = 0; i < 6; ++i) bits |= static_cast<rx::u64>(block[2 + i]) << (i * 8);
  for (u32 t = 0; t < 16; ++t) out16[t] = static_cast<u8>(palette[(bits >> (t * 3)) & 7u]);
}

void DecodeBc3(const u8* block, u8* rgba) {
  u8 alpha[16];
  DecodeBc4(block, alpha);
  DecodeBc1(block + 8, rgba);
  for (u32 t = 0; t < 16; ++t) rgba[t * 4 + 3] = alpha[t];
}

void DecodeBc5(const u8* block, u8* rgba) {
  u8 red[16];
  u8 green[16];
  DecodeBc4(block, red);
  DecodeBc4(block + 8, green);
  for (u32 t = 0; t < 16; ++t) {
    rgba[t * 4 + 0] = red[t];
    rgba[t * 4 + 1] = green[t];
    rgba[t * 4 + 2] = 0;
    rgba[t * 4 + 3] = 255;
  }
}

bool DecodeBc7Mode6(const u8* block, u8* rgba) {
  if ((block[0] & 0x7fu) != 0x40u) return false;
  static const u32 weights[16] = {0,  4,  9,  13, 17, 21, 26, 30,
                                  34, 38, 43, 47, 51, 55, 60, 64};
  u32 e0[4];
  u32 e1[4];
  const u32 p0 = Bits(block, 63, 1);
  const u32 p1 = Bits(block, 64, 1);
  for (u32 c = 0; c < 4; ++c) {
    e0[c] = (Bits(block, 7 + c * 14, 7) << 1) | p0;
    e1[c] = (Bits(block, 14 + c * 14, 7) << 1) | p1;
  }
  u32 offset = 65;
  for (u32 t = 0; t < 16; ++t) {
    const u32 count = t == 0 ? 3u : 4u;
    const u32 w = weights[Bits(block, offset, count)];
    offset += count;
    for (u32 c = 0; c < 4; ++c) {
      rgba[t * 4 + c] = static_cast<u8>(((64 - w) * e0[c] + w * e1[c] + 32) >> 6);
    }
  }
  return true;
}

// --- helpers ----------------------------------------------------------------

f64 Psnr(const std::vector<u8>& a, const std::vector<u8>& b, u32 channels, u32 stride) {
  f64 sum = 0;
  u32 count = 0;
  for (size_t t = 0; t * stride < a.size(); ++t) {
    for (u32 c = 0; c < channels; ++c) {
      const f64 d = static_cast<f64>(a[t * stride + c]) - static_cast<f64>(b[t * stride + c]);
      sum += d * d;
      ++count;
    }
  }
  if (count == 0) return 0;
  const f64 mse = sum / static_cast<f64>(count);
  if (mse <= 0.0) return 99.0;
  return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// Encodes a whole 4-multiple surface block by block and decodes it back, so the
// measurement covers the same edge cases a real texture hits.
using BlockEncode = void (*)(const u8*, u8*);
using BlockDecode = void (*)(const u8*, u8*);

std::vector<u8> RoundTrip(const std::vector<u8>& rgba, u32 width, u32 height, u32 block_bytes,
                          BlockEncode encode, BlockDecode decode) {
  std::vector<u8> out(rgba.size());
  u8 block[64];
  u8 packed[16];
  u8 decoded[64];
  for (u32 by = 0; by < height / 4; ++by) {
    for (u32 bx = 0; bx < width / 4; ++bx) {
      for (u32 ty = 0; ty < 4; ++ty) {
        for (u32 tx = 0; tx < 4; ++tx) {
          std::memcpy(block + (ty * 4 + tx) * 4,
                      rgba.data() + ((static_cast<size_t>(by) * 4 + ty) * width + bx * 4 + tx) * 4,
                      4);
        }
      }
      std::memset(packed, 0, sizeof(packed));
      encode(block, packed);
      (void)block_bytes;
      decode(packed, decoded);
      for (u32 ty = 0; ty < 4; ++ty) {
        for (u32 tx = 0; tx < 4; ++tx) {
          std::memcpy(out.data() + ((static_cast<size_t>(by) * 4 + ty) * width + bx * 4 + tx) * 4,
                      decoded + (ty * 4 + tx) * 4, 4);
        }
      }
    }
  }
  return out;
}

void DecodeBc7Wrapper(const u8* block, u8* rgba) {
  if (!DecodeBc7Mode6(block, rgba)) std::memset(rgba, 0, 64);
}

u32 Rand(u32& state) {
  state = state * 1664525u + 1013904223u;
  return state >> 8;
}

// A plausible albedo: low-frequency colour plus fine grain, which is the mix
// that separates a usable encoder from one that only handles flat blocks.
std::vector<u8> MakeAlbedo(u32 size) {
  std::vector<u8> image(static_cast<size_t>(size) * size * 4);
  u32 state = 12345;
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      const f32 u = static_cast<f32>(x) / static_cast<f32>(size);
      const f32 v = static_cast<f32>(y) / static_cast<f32>(size);
      const f32 grain = static_cast<f32>(Rand(state) % 32) - 16.0f;
      u8* p = image.data() + (static_cast<size_t>(y) * size + x) * 4;
      p[0] = static_cast<u8>(std::fmin(255.0f, std::fmax(0.0f, 180.0f * u + 40.0f + grain)));
      p[1] = static_cast<u8>(std::fmin(255.0f, std::fmax(0.0f, 140.0f * v + 60.0f + grain)));
      p[2] = static_cast<u8>(
          std::fmin(255.0f, std::fmax(0.0f, 90.0f * (u + v) * 0.5f + 30.0f + grain)));
      p[3] = 255;
    }
  }
  return image;
}

// A bumpy tangent-space normal map, encoded the way content ships it.
std::vector<u8> MakeNormalMap(u32 size) {
  std::vector<u8> image(static_cast<size_t>(size) * size * 4);
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      // Built the way content is: normalize the gradient of a height field.
      // That bounds the tilt (here about 50 degrees) instead of producing
      // near-horizontal normals, where z = sqrt(1 - x^2 - y^2) is so steep that
      // any format, this one included, turns a one-level xy error into a large
      // angle. Roughly the spatial frequency a 1k tiling material carries.
      const f32 u = static_cast<f32>(x) * 0.09f;
      const f32 v = static_cast<f32>(y) * 0.07f;
      f32 n[3] = {-(0.55f * std::cos(u) + 0.2f * std::cos(u * 3.1f + v)),
                  -(0.55f * std::sin(v) + 0.2f * std::sin(u * 2.3f)), 1.0f};
      const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      u8* p = image.data() + (static_cast<size_t>(y) * size + x) * 4;
      for (u32 c = 0; c < 3; ++c) {
        p[c] = static_cast<u8>((n[c] / len) * 127.5f + 127.5f);
      }
      p[3] = 255;
    }
  }
  return image;
}

void TestBc7Color() {
  const u32 size = 64;
  const std::vector<u8> source = MakeAlbedo(size);
  const std::vector<u8> decoded =
      RoundTrip(source, size, size, 16, asset::EncodeBc7Block, DecodeBc7Wrapper);
  const f64 psnr = Psnr(source, decoded, 3, 4);
  std::printf("bc_encode_test: BC7 albedo psnr %.2f dB\n", psnr);
  Check(psnr > 33.0, "BC7 albedo psnr below 33 dB");
}

void TestBc7BeatsBc1() {
  const u32 size = 64;
  const std::vector<u8> source = MakeAlbedo(size);
  const f64 bc7 =
      Psnr(source, RoundTrip(source, size, size, 16, asset::EncodeBc7Block, DecodeBc7Wrapper), 3, 4);
  const f64 bc1 =
      Psnr(source, RoundTrip(source, size, size, 8, asset::EncodeBc1Block, DecodeBc1), 3, 4);
  std::printf("bc_encode_test: BC1 albedo psnr %.2f dB\n", bc1);
  // The whole reason colour goes to BC7 and not to the half-size BC1.
  Check(bc7 > bc1 + 2.0, "BC7 is not clearly ahead of BC1 on albedo");
  Check(bc1 > 26.0, "BC1 albedo psnr below 26 dB");
}

void TestBc7GreyData() {
  // ORM-style data: grey values sit on the endpoint axis, so mode 6 should be
  // very nearly lossless. This is the case that justifies not using BC4/BC5 for
  // data maps.
  const u32 size = 64;
  std::vector<u8> source(static_cast<size_t>(size) * size * 4);
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      const u8 v = static_cast<u8>((x * 2 + y) * 255 / (size * 3 - 3));
      u8* p = source.data() + (static_cast<size_t>(y) * size + x) * 4;
      p[0] = 255;             // occlusion, unused
      p[1] = v;               // roughness
      p[2] = static_cast<u8>(127 - v);  // metallic
      p[3] = 255;
    }
  }
  const std::vector<u8> decoded =
      RoundTrip(source, size, size, 16, asset::EncodeBc7Block, DecodeBc7Wrapper);
  const f64 psnr = Psnr(source, decoded, 3, 4);
  std::printf("bc_encode_test: BC7 orm psnr %.2f dB\n", psnr);
  Check(psnr > 36.0, "BC7 orm psnr below 36 dB");
}

void TestBc5Normal() {
  const u32 size = 64;
  const std::vector<u8> source = MakeNormalMap(size);
  const std::vector<u8> decoded =
      RoundTrip(source, size, size, 16, asset::EncodeBc5Block, DecodeBc5);
  const f64 psnr = Psnr(source, decoded, 2, 4);
  std::printf("bc_encode_test: BC5 normal xy psnr %.2f dB\n", psnr);
  Check(psnr > 44.0, "BC5 normal xy psnr below 44 dB");

  // What actually matters is the angle after the shader reconstructs z, since
  // that is what lights the surface. Banded lighting shows up here long before
  // it shows up in a per-channel error.
  f64 worst_degrees = 0;
  f64 mean_degrees = 0;
  u32 samples = 0;
  for (size_t t = 0; t * 4 < source.size(); ++t) {
    auto decode = [](const u8* p, f32* n) {
      n[0] = static_cast<f32>(p[0]) / 127.5f - 1.0f;
      n[1] = static_cast<f32>(p[1]) / 127.5f - 1.0f;
      n[2] = std::sqrt(std::fmax(0.0f, 1.0f - n[0] * n[0] - n[1] * n[1]));
      const f32 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      for (u32 c = 0; c < 3; ++c) n[c] /= len;
    };
    f32 a[3];
    f32 b[3];
    decode(source.data() + t * 4, a);
    decode(decoded.data() + t * 4, b);
    const f32 dot = std::fmin(1.0f, std::fmax(-1.0f, a[0] * b[0] + a[1] * b[1] + a[2] * b[2]));
    const f64 degrees = std::acos(dot) * 57.2957795;
    worst_degrees = std::fmax(worst_degrees, degrees);
    mean_degrees += degrees;
    ++samples;
  }
  mean_degrees /= static_cast<f64>(samples);
  std::printf("bc_encode_test: BC5 normal angle mean %.3f deg, worst %.3f deg\n", mean_degrees,
              worst_degrees);
  // Banded lighting is a mean-error symptom; the worst texel is a single
  // pixel and gets filtered away.
  Check(mean_degrees < 0.6, "BC5 mean reconstructed normal angle above 0.6 degrees");
  Check(worst_degrees < 2.0, "BC5 worst reconstructed normal angle above 2 degrees");

  // And the comparison the format choice rests on.
  const std::vector<u8> as_bc7 =
      RoundTrip(source, size, size, 16, asset::EncodeBc7Block, DecodeBc7Wrapper);
  const f64 bc7_psnr = Psnr(source, as_bc7, 2, 4);
  std::printf("bc_encode_test: BC7 normal xy psnr %.2f dB\n", bc7_psnr);
  Check(psnr > bc7_psnr + 3.0, "BC5 is not clearly ahead of BC7 on a normal map");
}

void TestBc3Alpha() {
  const u32 size = 64;
  std::vector<u8> source(static_cast<size_t>(size) * size * 4);
  u32 state = 777;
  for (u32 y = 0; y < size; ++y) {
    for (u32 x = 0; x < size; ++x) {
      u8* p = source.data() + (static_cast<size_t>(y) * size + x) * 4;
      p[0] = static_cast<u8>(40 + (Rand(state) % 24));
      p[1] = static_cast<u8>(110 + (Rand(state) % 40));
      p[2] = static_cast<u8>(30 + (Rand(state) % 20));
      // A leaf-shaped cutout: hard edges are exactly what a shared colour/alpha
      // index set would smear.
      const f32 dx = static_cast<f32>(x) - 32.0f;
      const f32 dy = static_cast<f32>(y) - 32.0f;
      p[3] = dx * dx * 0.6f + dy * dy < 400.0f ? 255 : 0;
    }
  }
  const std::vector<u8> decoded =
      RoundTrip(source, size, size, 16, asset::EncodeBc3Block, DecodeBc3);
  u32 wrong = 0;
  for (size_t t = 0; t * 4 < source.size(); ++t) {
    const bool want = source[t * 4 + 3] >= 128;
    const bool got = decoded[t * 4 + 3] >= 128;
    if (want != got) ++wrong;
  }
  std::printf("bc_encode_test: BC3 cutout mismatches %u of %u\n", wrong,
              static_cast<u32>(source.size() / 4));
  Check(wrong == 0, "BC3 changed which texels pass a 0.5 alpha cutoff");
}

void TestBc7AlphaDecode() {
  // MaterialSystem's vegetation opacity bake reads alpha back off the encoded
  // block, so the encoder and that reader have to agree bit for bit.
  u8 block[64];
  u32 state = 99;
  for (u32 t = 0; t < 16; ++t) {
    block[t * 4 + 0] = static_cast<u8>(Rand(state) % 256);
    block[t * 4 + 1] = static_cast<u8>(Rand(state) % 256);
    block[t * 4 + 2] = static_cast<u8>(Rand(state) % 256);
    block[t * 4 + 3] = static_cast<u8>(t * 17);
  }
  u8 packed[16] = {};
  asset::EncodeBc7Block(block, packed);
  u8 reference[64];
  Check(DecodeBc7Mode6(packed, reference), "encoder did not emit BC7 mode 6");
  u8 decoded[64];
  Check(asset::DecodeBc7Block(packed, decoded), "DecodeBc7Block rejected a mode 6 block");
  Check(std::memcmp(decoded, reference, sizeof(decoded)) == 0,
        "DecodeBc7Block disagrees with an independent mode 6 decode");

  u8 not_mode6[16] = {};
  not_mode6[0] = 0x01;  // mode 0
  Check(!asset::DecodeBc7Block(not_mode6, decoded), "DecodeBc7Block accepted a non-mode-6 block");
}

void TestFlatBlocks() {
  // Degenerate blocks are where a principal-axis fit divides by zero. Every
  // format must return the constant exactly.
  for (u32 value = 0; value < 256; value += 51) {
    u8 block[64];
    for (u32 t = 0; t < 16; ++t) {
      block[t * 4 + 0] = static_cast<u8>(value);
      block[t * 4 + 1] = static_cast<u8>(value);
      block[t * 4 + 2] = static_cast<u8>(value);
      block[t * 4 + 3] = static_cast<u8>(value);
    }
    u8 packed[16] = {};
    u8 decoded[64];
    asset::EncodeBc7Block(block, packed);
    Check(DecodeBc7Mode6(packed, decoded), "flat BC7 block is not mode 6");
    bool exact = true;
    for (u32 t = 0; t < 16; ++t) {
      for (u32 c = 0; c < 4; ++c) {
        if (decoded[t * 4 + c] != value) exact = false;
      }
    }
    Check(exact, "BC7 did not reproduce a flat block exactly");

    std::memset(packed, 0, sizeof(packed));
    asset::EncodeBc5Block(block, packed);
    DecodeBc5(packed, decoded);
    Check(decoded[0] == value && decoded[1] == value, "BC5 did not reproduce a flat block exactly");
  }
}

// --- CompressTexture --------------------------------------------------------

asset::Texture MakeTexture(const std::vector<u8>& rgba, u32 size, bool srgb) {
  asset::Texture texture;
  texture.format = asset::TextureFormat::kRgba8;
  texture.width = size;
  texture.height = size;
  texture.is_srgb = srgb;
  texture.data.resize(rgba.size());
  std::memcpy(texture.data.data(), rgba.data(), rgba.size());
  return texture;
}

void TestCompressTexture() {
  namespace fs = std::filesystem;
  const fs::path cache = fs::temp_directory_path() / "rx_bc_encode_test_cache";
  std::error_code error;
  fs::remove_all(cache, error);
#if defined(_WIN32)
  _putenv_s("RX_TEXCACHE_DIR", cache.string().c_str());
#else
  setenv("RX_TEXCACHE_DIR", cache.string().c_str(), 1);
#endif

  const u32 size = 64;
  const std::vector<u8> albedo = MakeAlbedo(size);

  // Off by default: with no gpu nothing may start emitting block formats.
  asset::Texture untouched = MakeTexture(albedo, size, true);
  Check(!asset::CompressTexture(&untouched, asset::TextureRole::kColor, "test/albedo"),
        "compression ran while unsupported");
  Check(untouched.format == asset::TextureFormat::kRgba8, "unsupported path modified the texture");

  // Normal maps are opt-in even once the device supports BC.
  asset::SetTextureCompression({.supported = true, .normals = false});
  asset::Texture skipped_normal = MakeTexture(MakeNormalMap(size), size, false);
  Check(!asset::CompressTexture(&skipped_normal, asset::TextureRole::kNormalTangent,
                                "test/normal-off"),
        "normal compressed with the normals option off");
  Check(skipped_normal.format == asset::TextureFormat::kRgba8,
        "skipped normal map was modified");

  asset::SetTextureCompression({.supported = true, .normals = true});

  asset::Texture color = MakeTexture(albedo, size, true);
  Check(asset::CompressTexture(&color, asset::TextureRole::kColor, "test/albedo"),
        "opaque colour did not compress");
  Check(color.format == asset::TextureFormat::kBc7, "opaque colour did not pick BC7");
  Check(color.mip_count == 7, "a 64x64 chain should be 7 levels");
  Check(color.data.size() == 4096 + 1024 + 256 + 64 + 16 + 16 + 16,
        "BC7 chain size does not match the block layout");

  asset::Texture normal = MakeTexture(MakeNormalMap(size), size, false);
  Check(asset::CompressTexture(&normal, asset::TextureRole::kNormalTangent, "test/normal"),
        "normal did not compress");
  Check(normal.format == asset::TextureFormat::kBc5, "normal did not pick BC5");

  asset::Texture data = MakeTexture(albedo, size, false);
  Check(asset::CompressTexture(&data, asset::TextureRole::kData, "test/data"),
        "data did not compress");
  Check(data.format == asset::TextureFormat::kBc7, "data did not pick BC7");

  std::vector<u8> masked = albedo;
  masked[3] = 0;
  asset::Texture cutout = MakeTexture(masked, size, true);
  Check(asset::CompressTexture(&cutout, asset::TextureRole::kColor, "test/cutout"),
        "cutout colour did not compress");
  Check(cutout.format == asset::TextureFormat::kBc3, "colour with alpha did not pick BC3");

  // Already compressed: a second pass must not re-encode block data as if it
  // were rgba8.
  asset::Texture again = color;
  Check(!asset::CompressTexture(&again, asset::TextureRole::kColor, "test/albedo"),
        "compressed texture was compressed again");

  // Edges that are not a multiple of four stay rgba8 rather than getting
  // silently resized.
  asset::Texture odd;
  odd.format = asset::TextureFormat::kRgba8;
  odd.width = 30;
  odd.height = 30;
  odd.data.resize(30 * 30 * 4);
  Check(!asset::CompressTexture(&odd, asset::TextureRole::kData, "test/odd"),
        "a 30x30 texture compressed");
  Check(odd.format == asset::TextureFormat::kRgba8, "skipped texture was modified");

  // The cache has to return the same bytes, and be keyed on content: the same
  // pixels under a different identity must hit.
  const asset::TextureCompressionStats before = asset::CompressionTotals();
  asset::Texture cached = MakeTexture(albedo, size, true);
  Check(asset::CompressTexture(&cached, asset::TextureRole::kColor, "test/albedo-elsewhere"),
        "cached reload failed");
  const asset::TextureCompressionStats after = asset::CompressionTotals();
  Check(after.cache_hits == before.cache_hits + 1, "content-keyed cache did not hit");
  Check(cached.data.size() == color.data.size() &&
            std::memcmp(cached.data.data(), color.data.data(), color.data.size()) == 0,
        "cache returned different bytes than the encoder");

  asset::SetTextureCompression({});
  fs::remove_all(cache, error);
}

}  // namespace

int main() {
  TestBc7Color();
  TestBc7BeatsBc1();
  TestBc7GreyData();
  TestBc5Normal();
  TestBc3Alpha();
  TestBc7AlphaDecode();
  TestFlatBlocks();
  TestCompressTexture();
  if (failures == 0) std::printf("bc_encode_test: ok\n");
  return failures == 0 ? 0 : 1;
}
