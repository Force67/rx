// rxdiff -- compare two rendered pngs and fail when they differ by more than a
// tolerance.
//
//   rxdiff <a.png> <b.png> [--rmse <t>] [--hot <f>] [--hot-delta <d>]
//          [--diff <out.png>] [--json]
//
// The renderer is NOT deterministic frame to frame: TAA jitters the sample
// position on a per-frame sequence and the temporal history resolves against
// whatever the previous frames happened to be, so two runs of the same build on
// the same scene write different bytes. Hashing a capture is therefore useless
// and "pixel identical" is a claim nothing here can make. What IS stable is how
// far apart two runs land, which is what this measures.
//
// Two numbers, because content goes wrong in two shapes:
//
//   rmse   root mean square error over the rgb channels, 0..1. Sees a change
//          spread across the frame (exposure, a light, a material) that no
//          single pixel makes obvious.
//   hot    the fraction of pixels whose worst channel moved by more than
//          --hot-delta. Sees one small object going wrong in a large frame,
//          which is exactly what an average washes out.
//
// Exit 0 when both are within tolerance, 1 when either is not, 2 on a usage or
// io error (an unreadable file must not read as "no difference").

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

namespace {

// The defaults below are measured, not guessed. 73 pairs of captures, each pair
// two runs of the SAME build on the SAME scene with the same flags:
//
//   cornell        640x360  f8   vkrun            28 pairs  rmse max 0.00287
//   showcase      1280x720  f20  vkrun            15 pairs  rmse max 0.00375
//   material_sheet 1280x960 f20  vkrun            15 pairs  rmse max 0.00152
//   cornell        640x360  f8   swrun --no-rt    15 pairs  rmse max 0.00229
//
// so the run-to-run noise floor of this renderer is rmse 0.00375, with single
// channel excursions as large as 0.13 on a high contrast edge.

// How far one channel has to move for a pixel to count as hot. Deliberately far
// above "visible to the eye": at 0.031 (a step you can see on a gradient) the
// noise alone lights up 0.8% of showcase, and at 0.10 it still lights up 18
// pixels of cornell. From 0.15 up, all 73 pairs above produce EXACTLY zero hot
// pixels; 0.25 takes that with margin. That zero is the whole value of this
// metric: any hot pixel at all is content, not jitter.
constexpr float kDefaultHotDelta = 0.25f;

// Twice the worst floor above. Not tighter: the floor itself spreads 3x between
// the quietest and busiest scene measured, so a scene busier than these has room
// to sit above 0.00375, and a gate that flakes is a gate the next agent learns
// to ignore. The cost is stated plainly rather than hidden: changing one Cornell
// wall's albedo by 12% scores 0.00700 and passes. This resolves changes from
// about 2x the floor up (moving a 0.35 m ball 5 cm scores 0.01999, shrinking it
// to 0.30 m scores 0.02575); below that the renderer's own wander is the same
// size as the change, and no threshold makes that not true. Pass --rmse to
// tighten it on a scene whose own floor you have measured.
constexpr float kDefaultRmse = 0.0075f;

// The measured floor for this one is exactly zero, so the default is pure
// headroom for a scene noisier than the four above rather than a margin over
// anything observed: 0.01% of the frame is 23 pixels at 640x360 and 92 at
// 1280x720, roughly a 10x10 object gone wrong.
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
