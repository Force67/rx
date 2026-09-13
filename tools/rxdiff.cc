// rxdiff: compare two rendered pngs and fail past a tolerance.
//
//   rxdiff <a.png> <b.png> [--rmse <t>] [--hot <f>] [--hot-delta <d>]
//          [--diff <out.png>] [--json]
//
// Both captures must come from the locked capture clock (rx --shot fixes 1/60 s
// per frame). Two wall-clock captures land ~25x further apart than the defaults
// allow. Software and raytracing-off runs are bit identical run to run; with
// ray tracing on, rcgi's hash-slot claim order leaves a least-significant-bit
// residual, so hashing a capture is never a check to rely on. Measured floor:
// rmse 0.000553 over 78 same-build pairs (cornell / showcase / material_sheet /
// model, 3 resolutions, 3 frame counts, vkrun + swrun, --no-taa and --no-rt);
// it does not grow with frame count or resolution, and RX_RCGI=0 makes even a
// raytraced capture bit identical.
//
// Two metrics, because content fails in two shapes:
//   rmse  over rgb (0..1): sees change spread across the frame (exposure,
//         light, material) that no single pixel makes obvious.
//   hot   fraction of pixels whose worst channel moved more than --hot-delta:
//         sees one small object going wrong in a large frame.
//
// Exit 0 within tolerance, 1 when either metric is over, 2 on usage/io error.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

namespace {

// Measured provenance for these defaults is summarized in the file header.
// Floor: rmse 0.000553 (rcgi hash-slot claim order; --no-rt and swrun pairs are
// bit identical), worst single-channel excursion 0.0275.

// Above the measured noise: 0.03 lights up exactly zero pixels across all 78
// pairs, so 0.10 keeps that zero with 3x to spare. Any hot pixel is content.
constexpr float kDefaultHotDelta = 0.10f;

// 4.6x the measured floor; the headroom is for gpu-side residuals this vendor
// does not dither. One Cornell wall's albedo 0.8 -> 0.7 scores 0.00689 and
// FAILS (it passed the old wall-clock gate); sensitivity runs to ~4% albedo on
// one wall, and geometry is louder still (a 5 cm move scores 0.01989).
constexpr float kDefaultRmse = 0.002f;

// Measured floor is exactly zero at kDefaultHotDelta; pure headroom for a
// noisier scene (0.01% of the frame is a ~10x10 object gone wrong).
constexpr float kDefaultHot = 0.0001f;

struct Image {
  int width = 0;
  int height = 0;
  unsigned char* pixels = nullptr;  // rgb8, stbi-owned
};

int Fail(const std::string& message) {
  std::fprintf(stderr, "rxdiff: %s\n", message.c_str());
  return 2;
}

// Forced to 3 channels: a capture's alpha is not part of what anyone is
// comparing, and it lets an rgb and an rgba png of the same frame compare.
bool Load(const char* path, Image* out) {
  int channels = 0;
  out->pixels = stbi_load(path, &out->width, &out->height, &channels, 3);
  return out->pixels != nullptr;
}

struct Result {
  double rmse = 0.0;
  double max_delta = 0.0;
  long long hot_pixels = 0;
  long long total_pixels = 0;
  // Bounds of the hot pixels, so a failure says WHERE. Empty when none are hot.
  int min_x = 0, min_y = 0, max_x = -1, max_y = -1;
};

Result Compare(const Image& a, const Image& b, float hot_delta,
               std::vector<unsigned char>* diff) {
  Result result;
  result.total_pixels = static_cast<long long>(a.width) * a.height;
  if (diff) diff->assign(static_cast<size_t>(result.total_pixels) * 3, 0);
  result.min_x = a.width;
  result.min_y = a.height;

  double sum_squares = 0.0;
  for (int y = 0; y < a.height; ++y) {
    for (int x = 0; x < a.width; ++x) {
      const size_t base = (static_cast<size_t>(y) * a.width + x) * 3;
      float worst = 0.0f;
      for (int c = 0; c < 3; ++c) {
        const float delta =
            (static_cast<float>(a.pixels[base + c]) - static_cast<float>(b.pixels[base + c])) /
            255.0f;
        sum_squares += static_cast<double>(delta) * delta;
        const float magnitude = std::fabs(delta);
        if (magnitude > worst) worst = magnitude;
        // Amplified so a difference the eye cannot find in the source frames is
        // obvious in the diff; saturating is the point, not a defect.
        if (diff) {
          const float amplified = magnitude * 8.0f * 255.0f;
          (*diff)[base + c] = static_cast<unsigned char>(amplified > 255.0f ? 255.0f : amplified);
        }
      }
      if (worst > result.max_delta) result.max_delta = worst;
      if (worst <= hot_delta) continue;
      ++result.hot_pixels;
      if (x < result.min_x) result.min_x = x;
      if (y < result.min_y) result.min_y = y;
      if (x > result.max_x) result.max_x = x;
      if (y > result.max_y) result.max_y = y;
    }
  }
  result.rmse = std::sqrt(sum_squares / (static_cast<double>(result.total_pixels) * 3.0));
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path_a = nullptr;
  const char* path_b = nullptr;
  const char* diff_path = nullptr;
  float rmse_limit = kDefaultRmse;
  float hot_limit = kDefaultHot;
  float hot_delta = kDefaultHotDelta;
  bool json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (arg == "--rmse") rmse_limit = std::strtof(next(), nullptr);
    else if (arg == "--hot") hot_limit = std::strtof(next(), nullptr);
    else if (arg == "--hot-delta") hot_delta = std::strtof(next(), nullptr);
    else if (arg == "--diff") diff_path = next();
    else if (arg == "--json") json = true;
    else if (!path_a) path_a = argv[i];
    else if (!path_b) path_b = argv[i];
    else return Fail("unexpected argument '" + arg + "'");
  }
  if (!path_a || !path_b) {
    std::fprintf(stderr,
                 "usage: rxdiff <a.png> <b.png> [--rmse <t>] [--hot <f>] [--hot-delta <d>]\n"
                 "               [--diff <out.png>] [--json]\n"
                 "  defaults: --rmse %g --hot %g --hot-delta %g, measured against this "
                 "renderer's\n  own run-to-run noise (see the top of tools/rxdiff.cc)\n",
                 static_cast<double>(kDefaultRmse), static_cast<double>(kDefaultHot),
                 static_cast<double>(kDefaultHotDelta));
    return 2;
  }

  Image a;
  Image b;
  if (!Load(path_a, &a)) return Fail(std::string("cannot read '") + path_a + "'");
  if (!Load(path_b, &b)) return Fail(std::string("cannot read '") + path_b + "'");
  if (a.width != b.width || a.height != b.height) {
    char message[160];
    std::snprintf(message, sizeof(message), "size mismatch: %dx%d vs %dx%d", a.width, a.height,
                  b.width, b.height);
    return Fail(message);
  }

  std::vector<unsigned char> diff;
  const Result result = Compare(a, b, hot_delta, diff_path ? &diff : nullptr);
  if (diff_path && !stbi_write_png(diff_path, a.width, a.height, 3, diff.data(), a.width * 3)) {
    return Fail(std::string("cannot write '") + diff_path + "'");
  }

  const double hot_fraction =
      static_cast<double>(result.hot_pixels) / static_cast<double>(result.total_pixels);
  const bool pass = result.rmse <= rmse_limit && hot_fraction <= hot_limit;

  if (json) {
    std::printf("{\n  \"a\": \"%s\",\n  \"b\": \"%s\",\n  \"width\": %d,\n  \"height\": %d,\n",
                path_a, path_b, a.width, a.height);
    std::printf("  \"rmse\": %.6f,\n  \"rmse_limit\": %.6f,\n", result.rmse,
                static_cast<double>(rmse_limit));
    std::printf("  \"max_delta\": %.6f,\n  \"hot_pixels\": %lld,\n  \"hot_fraction\": %.6f,\n"
                "  \"hot_limit\": %.6f,\n",
                result.max_delta, result.hot_pixels, hot_fraction,
                static_cast<double>(hot_limit));
    if (result.max_x >= result.min_x) {
      std::printf("  \"hot_bounds\": {\"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d},\n",
                  result.min_x, result.min_y, result.max_x - result.min_x + 1,
                  result.max_y - result.min_y + 1);
    }
    std::printf("  \"pass\": %s\n}\n", pass ? "true" : "false");
  } else {
    std::printf("%s vs %s (%dx%d): rmse %.5f (limit %.5f), max delta %.5f, "
                "%lld hot pixel(s) = %.4f%% (limit %.4f%%)\n",
                path_a, path_b, a.width, a.height, result.rmse,
                static_cast<double>(rmse_limit), result.max_delta, result.hot_pixels,
                hot_fraction * 100.0, static_cast<double>(hot_limit) * 100.0);
    if (result.max_x >= result.min_x) {
      std::printf("  hot region: %dx%d at %d,%d\n", result.max_x - result.min_x + 1,
                  result.max_y - result.min_y + 1, result.min_x, result.min_y);
    }
    std::printf("  %s\n", pass ? "PASS" : "FAIL");
  }

  stbi_image_free(a.pixels);
  stbi_image_free(b.pixels);
  return pass ? 0 : 1;
}
