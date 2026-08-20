// rxdiff -- compare two rendered pngs and fail when they differ by more than a
// tolerance.
//
//   rxdiff <a.png> <b.png> [--rmse <t>] [--hot <f>] [--hot-delta <d>]
//          [--diff <out.png>] [--json]
//
// A capture run (rx --shot) locks the clock to a fixed 1/60 s delta, so what a
// frame contains is a function of the frame index and not of how busy the
// machine was. Every software and raytracing-off configuration measured below
// then came out BIT identical run to run; with raytracing on a residual of a
// few least significant bits survives in the traced gi, so hashing a capture is
// still not a check to rely on. What IS stable is how far apart two runs land,
// which is what this measures.
//
// The tolerances below assume both captures came from that locked clock. Two
// wall-clock captures (RX_FIXED_DT=0, or a screenshot grabbed out of a windowed
// session) land ~25x further apart than the defaults allow, because the frames
// leading up to the capture advanced by different deltas.
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

// The defaults below are measured, not guessed. 78 pairs of captures, each pair
// two --shot runs of the SAME build on the SAME scene with the same flags:
//
//   cornell        640x360  f8   vkrun            6 pairs  rmse max 0.000194
//   cornell       1280x720  f20  vkrun            6 pairs  rmse max 0.000433
//   cornell        640x360  f60  vkrun            6 pairs  rmse max 0.000151
//   showcase       640x360  f8   vkrun            6 pairs  rmse max 0.000276
//   showcase      1280x720  f20  vkrun            6 pairs  rmse max 0.000192
//   showcase       640x360  f60  vkrun            6 pairs  rmse max 0.000137
//   showcase      1920x1080 f30  vkrun            3 pairs  rmse max 0.000104
//   material_sheet 640x480  f8   vkrun            6 pairs  rmse max 0.000185
//   material_sheet 1280x960 f20  vkrun            6 pairs  rmse max 0.000203
//   model          640x360  f30  vkrun            6 pairs  rmse max 0.000163
//   model         1280x720  f20  vkrun            6 pairs  rmse max 0.000082
//   showcase+cornell 640x360 f8  vkrun --no-taa   6 pairs  rmse max 0.000254
//   cornell        640x360  f8   vkrun --no-rt    3 pairs  rmse max 0 (identical)
//   cornell        640x360  f8   swrun --no-rt    3 pairs  rmse max 0 (identical)
//   showcase       640x360  f8   swrun --no-rt    3 pairs  rmse max 0 (identical)
//
// so the floor is rmse 0.000433, with single channel excursions no larger than
// 0.0275 (7/255). It does not grow with frame count or resolution, and it is
// the radiance cache alone: RX_RCGI=0 makes even a raytraced capture bit
// identical with rtao, ddgi and the NRD denoisers still running, so what is
// left is rcgi's hash slots being claimed in whatever order the waves land in
// (InterlockedCompareExchange in shaders/gi/rcgi_probe_trace_body.hlsli), not
// anything time- or load-dependent. --no-taa does not move it either: TAA was
// covering for the wall clock, not for the tracer.

// How far one channel has to move for a pixel to count as hot. Above the noise
// by construction: across the 78 pairs a delta of 0.02 lights up 2 pixels in
// the whole set and 0.03 up lights up EXACTLY zero, so 0.10 keeps that zero
// with 3x to spare and is still 2.5x tighter than the old 0.25. That zero is
// the whole value of this metric: any hot pixel at all is content, not jitter.
constexpr float kDefaultHotDelta = 0.10f;

// 4.6x the worst floor above, and the number the gate lives or dies by. The
// headroom is deliberately larger than the spread measured here (the busiest
// pair is 16x the quietest, but all of it under 0.0005) because the
// residual is gpu-side: another vendor's tracer may dither more pixels than
// this one does, and a gate that flakes is a gate the next agent learns to
// ignore. Even a quarter of the frame moving by one lsb stays under it.
//
// What it buys: one Cornell wall's albedo dropped 0.8 -> 0.7 scores 0.00689 and
// FAILS, at 3.4x the limit. That change passed the old 0.0075 gate, which is
// what the locked capture clock was for. Sensitivity now runs down to about a
// 4% albedo change on one wall: 0.8 -> 0.77 scores 0.00216 and fails, 0.8 ->
// 0.78 scores 0.00155 and does not. Geometry is louder still: moving a 0.35 m
// ball 5 cm scores 0.01989, shrinking it to 0.30 m scores 0.02566.
constexpr float kDefaultRmse = 0.002f;

// The measured floor for this one is exactly zero at the delta above, so the
// default is pure headroom for a scene noisier than the ones measured rather
// than a margin over anything observed: 0.01% of the frame is 23 pixels at
// 640x360 and 92 at 1280x720, roughly a 10x10 object gone wrong.
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
