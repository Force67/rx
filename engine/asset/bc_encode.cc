#include "asset/bc_encode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rx::asset {
namespace {

// BC7 4-bit index ramp, in 64ths (the spec's aWeight4).
constexpr u32 kWeight4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
// Fraction of the way from endpoint 0 to endpoint 1 for each BC1 colour index.
// Index 1 is the FAR endpoint, not the second step: that ordering is the
// format's, and getting it wrong swaps two of the four palette entries.
constexpr f32 kBc1Weight[4] = {0.0f, 1.0f, 1.0f / 3.0f, 2.0f / 3.0f};
// Same for BC4's 8-value mode (index 1 is again the far endpoint).
constexpr f32 kBc4Weight[8] = {0.0f,       1.0f,       1.0f / 7.0f, 2.0f / 7.0f,
                               3.0f / 7.0f, 4.0f / 7.0f, 5.0f / 7.0f, 6.0f / 7.0f};

// Endpoints for the best-fit line through the block, as the extremes of the
// texels projected onto the principal axis. N is 3 (rgb) or 4 (rgba); `points`
// is 16 texels of N floats.
template <int N>
void FitLine(const f32* points, f32* e0, f32* e1) {
  f32 mean[N] = {};
  for (u32 i = 0; i < kBlockTexels; ++i) {
    for (int c = 0; c < N; ++c) mean[c] += points[i * N + c];
  }
  for (int c = 0; c < N; ++c) mean[c] *= 1.0f / f32(kBlockTexels);

  f32 cov[N][N] = {};
  for (u32 i = 0; i < kBlockTexels; ++i) {
    f32 d[N];
    for (int c = 0; c < N; ++c) d[c] = points[i * N + c] - mean[c];
    for (int a = 0; a < N; ++a) {
      for (int b = 0; b < N; ++b) cov[a][b] += d[a] * d[b];
    }
  }

  // Seeded from the bounding-box diagonal: it is already the answer for most
  // blocks, and it keeps power iteration off the zero vector on a flat one.
  f32 axis[N];
  {
    f32 lo[N], hi[N];
    for (int c = 0; c < N; ++c) {
      lo[c] = points[c];
      hi[c] = points[c];
    }
    for (u32 i = 1; i < kBlockTexels; ++i) {
      for (int c = 0; c < N; ++c) {
        lo[c] = std::min(lo[c], points[i * N + c]);
        hi[c] = std::max(hi[c], points[i * N + c]);
      }
    }
    f32 len = 0;
    for (int c = 0; c < N; ++c) {
      axis[c] = hi[c] - lo[c];
      len += axis[c] * axis[c];
    }
    if (len < 1e-12f) {
      // Single-colour block: any axis works, the projection collapses anyway.
      for (int c = 0; c < N; ++c) axis[c] = c == 0 ? 1.0f : 0.0f;
    } else {
      const f32 inv = 1.0f / std::sqrt(len);
      for (int c = 0; c < N; ++c) axis[c] *= inv;
    }
  }
  for (int it = 0; it < 8; ++it) {
    f32 next[N] = {};
    for (int a = 0; a < N; ++a) {
      for (int b = 0; b < N; ++b) next[a] += cov[a][b] * axis[b];
    }
    f32 len = 0;
    for (int c = 0; c < N; ++c) len += next[c] * next[c];
    if (len < 1e-12f) break;
    const f32 inv = 1.0f / std::sqrt(len);
    for (int c = 0; c < N; ++c) axis[c] = next[c] * inv;
  }

  f32 tmin = 1e30f;
  f32 tmax = -1e30f;
  for (u32 i = 0; i < kBlockTexels; ++i) {
    f32 t = 0;
    for (int c = 0; c < N; ++c) t += (points[i * N + c] - mean[c]) * axis[c];
    tmin = std::min(tmin, t);
    tmax = std::max(tmax, t);
  }
  for (int c = 0; c < N; ++c) {
    e0[c] = std::clamp(mean[c] + axis[c] * tmin, 0.0f, 255.0f);
    e1[c] = std::clamp(mean[c] + axis[c] * tmax, 0.0f, 255.0f);
  }
}

// Least-squares endpoints for the interpolation weights the current indices
// imply (w = 0 lands on e0, w = 1 on e1). Leaves the endpoints alone when the
// system is singular, which happens exactly when every texel picked the same
// index and there is nothing to solve for.
template <int N>
void RefitEndpoints(const f32* points, const f32* weights, f32* e0, f32* e1) {
  f32 aa = 0;
  f32 ab = 0;
  f32 bb = 0;
  f32 ax[N] = {};
  f32 bx[N] = {};
  for (u32 i = 0; i < kBlockTexels; ++i) {
    const f32 b = weights[i];
    const f32 a = 1.0f - b;
    aa += a * a;
    ab += a * b;
    bb += b * b;
    for (int c = 0; c < N; ++c) {
      ax[c] += a * points[i * N + c];
      bx[c] += b * points[i * N + c];
    }
  }
  const f32 det = aa * bb - ab * ab;
  if (std::abs(det) < 1e-6f) return;
  const f32 inv = 1.0f / det;
  for (int c = 0; c < N; ++c) {
    e0[c] = std::clamp((bb * ax[c] - ab * bx[c]) * inv, 0.0f, 255.0f);
    e1[c] = std::clamp((aa * bx[c] - ab * ax[c]) * inv, 0.0f, 255.0f);
  }
}

// --- BC1 colour block ---

u16 Quantize565(const f32* c) {
  const int r = std::clamp(static_cast<int>(c[0] * (31.0f / 255.0f) + 0.5f), 0, 31);
  const int g = std::clamp(static_cast<int>(c[1] * (63.0f / 255.0f) + 0.5f), 0, 63);
  const int b = std::clamp(static_cast<int>(c[2] * (31.0f / 255.0f) + 0.5f), 0, 31);
  return static_cast<u16>((r << 11) | (g << 5) | b);
}

void Unquantize565(u16 v, f32* out) {
  const int r = (v >> 11) & 31;
  const int g = (v >> 5) & 63;
  const int b = v & 31;
  out[0] = static_cast<f32>((r << 3) | (r >> 2));
  out[1] = static_cast<f32>((g << 2) | (g >> 4));
  out[2] = static_cast<f32>((b << 3) | (b >> 2));
}

struct ColorFit {
  u16 c0 = 0;
  u16 c1 = 0;
  u8 index[kBlockTexels] = {};
};

// Quantizes the endpoint pair, orders it into the 4-colour form and assigns
// indices. `f0`/`f1` are swapped to match the packed order so the caller's
// next refit stays consistent with the indices it just got.
f32 FitColor(const f32* points, f32* f0, f32* f1, ColorFit* fit) {
  u16 q0 = Quantize565(f0);
  u16 q1 = Quantize565(f1);
  if (q0 < q1) {
    std::swap(q0, q1);
    for (int c = 0; c < 3; ++c) std::swap(f0[c], f1[c]);
  }
  f32 palette[4][3];
  Unquantize565(q0, palette[0]);
  Unquantize565(q1, palette[1]);
  for (int c = 0; c < 3; ++c) {
    palette[2][c] = (2.0f * palette[0][c] + palette[1][c]) * (1.0f / 3.0f);
    palette[3][c] = (palette[0][c] + 2.0f * palette[1][c]) * (1.0f / 3.0f);
  }
  f32 total = 0;
  for (u32 i = 0; i < kBlockTexels; ++i) {
    f32 best = 1e30f;
    u32 best_index = 0;
    for (u32 k = 0; k < 4; ++k) {
      f32 err = 0;
      for (int c = 0; c < 3; ++c) {
        const f32 d = points[i * 3 + c] - palette[k][c];
        err += d * d;
      }
      if (err < best) {
        best = err;
        best_index = k;
      }
    }
    fit->index[i] = static_cast<u8>(best_index);
    total += best;
  }
  fit->c0 = q0;
  fit->c1 = q1;
  return total;
}

void PackColorBlock(const ColorFit& fit, u8* out) {
  out[0] = static_cast<u8>(fit.c0 & 0xffu);
  out[1] = static_cast<u8>(fit.c0 >> 8);
  out[2] = static_cast<u8>(fit.c1 & 0xffu);
  out[3] = static_cast<u8>(fit.c1 >> 8);
  u32 bits = 0;
  for (u32 i = 0; i < kBlockTexels; ++i) bits |= static_cast<u32>(fit.index[i]) << (i * 2);
  out[4] = static_cast<u8>(bits & 0xffu);
  out[5] = static_cast<u8>((bits >> 8) & 0xffu);
  out[6] = static_cast<u8>((bits >> 16) & 0xffu);
  out[7] = static_cast<u8>((bits >> 24) & 0xffu);
}

void EncodeColorBlock(const u8* rgba, u8* out) {
  f32 points[kBlockTexels * 3];
  for (u32 i = 0; i < kBlockTexels; ++i) {
    for (int c = 0; c < 3; ++c) points[i * 3 + c] = static_cast<f32>(rgba[i * 4 + c]);
  }
  f32 e0[3];
  f32 e1[3];
  FitLine<3>(points, e0, e1);

  ColorFit best;
  f32 best_error = FitColor(points, e0, e1, &best);
  for (int pass = 0; pass < 2; ++pass) {
    f32 weights[kBlockTexels];
    for (u32 i = 0; i < kBlockTexels; ++i) weights[i] = kBc1Weight[best.index[i]];
    f32 n0[3] = {e0[0], e0[1], e0[2]};
    f32 n1[3] = {e1[0], e1[1], e1[2]};
    RefitEndpoints<3>(points, weights, n0, n1);
    ColorFit candidate;
    const f32 error = FitColor(points, n0, n1, &candidate);
    if (error >= best_error) break;
    best_error = error;
    best = candidate;
    std::memcpy(e0, n0, sizeof(e0));
    std::memcpy(e1, n1, sizeof(e1));
  }
  PackColorBlock(best, out);
}

// --- BC4 single-channel block ---

struct AlphaFit {
  u8 r0 = 0;
  u8 r1 = 0;
  u8 index[kBlockTexels] = {};
};

f32 FitAlpha(const f32* values, f32* f0, f32* f1, AlphaFit* fit) {
  // The 8-value mode is selected by r0 > r1; r0 is the endpoint index 0 lands
  // on, so the pair is ordered high-first.
  int q0 = std::clamp(static_cast<int>(*f0 + 0.5f), 0, 255);
  int q1 = std::clamp(static_cast<int>(*f1 + 0.5f), 0, 255);
  if (q0 < q1) {
    std::swap(q0, q1);
    std::swap(*f0, *f1);
  }
  f32 palette[8];
  palette[0] = static_cast<f32>(q0);
  palette[1] = static_cast<f32>(q1);
  for (u32 k = 2; k < 8; ++k) {
    palette[k] = (static_cast<f32>((8 - k) * q0 + (k - 1) * q1)) * (1.0f / 7.0f);
  }
  f32 total = 0;
  for (u32 i = 0; i < kBlockTexels; ++i) {
    f32 best = 1e30f;
    u32 best_index = 0;
    for (u32 k = 0; k < 8; ++k) {
      const f32 d = values[i] - palette[k];
      const f32 err = d * d;
      if (err < best) {
        best = err;
        best_index = k;
      }
    }
    fit->index[i] = static_cast<u8>(best_index);
    total += best;
  }
  fit->r0 = static_cast<u8>(q0);
  fit->r1 = static_cast<u8>(q1);
  return total;
}

void PackAlphaBlock(const AlphaFit& fit, u8* out) {
  out[0] = fit.r0;
  out[1] = fit.r1;
  u64 bits = 0;
  // A degenerate pair selects the 6-value mode, whose palette entries 6 and 7
  // are the hard 0 and 255 rather than interpolants. Every texel is on the one
  // endpoint there, so index 0 is both correct and what FitAlpha produced.
  for (u32 i = 0; i < kBlockTexels; ++i) bits |= static_cast<u64>(fit.index[i]) << (i * 3);
  for (u32 i = 0; i < 6; ++i) out[2 + i] = static_cast<u8>((bits >> (i * 8)) & 0xffu);
}

void EncodeChannelBlock(const f32* values, u8* out) {
  f32 lo = values[0];
  f32 hi = values[0];
  for (u32 i = 1; i < kBlockTexels; ++i) {
    lo = std::min(lo, values[i]);
    hi = std::max(hi, values[i]);
  }
  f32 e0 = hi;
  f32 e1 = lo;
  AlphaFit best;
  f32 best_error = FitAlpha(values, &e0, &e1, &best);
  for (int pass = 0; pass < 2; ++pass) {
    f32 weights[kBlockTexels];
    for (u32 i = 0; i < kBlockTexels; ++i) weights[i] = kBc4Weight[best.index[i]];
    f32 n0 = e0;
    f32 n1 = e1;
    RefitEndpoints<1>(values, weights, &n0, &n1);
    AlphaFit candidate;
    const f32 error = FitAlpha(values, &n0, &n1, &candidate);
    if (error >= best_error) break;
    best_error = error;
    best = candidate;
    e0 = n0;
    e1 = n1;
  }
  PackAlphaBlock(best, out);
}

// --- BC7 mode 6 ---

// The endpoint's low bit is the shared p-bit, so a component quantizes to the
// nearest 8-bit value of the requested parity.
u8 QuantizeWithParity(f32 v, u32 p) {
  const int q = std::clamp(static_cast<int>((v - static_cast<f32>(p)) * 0.5f + 0.5f), 0, 127);
  return static_cast<u8>((q << 1) | p);
}

f32 AssignBc7Indices(const f32* points, const u8* e0, const u8* e1, u8* index) {
  f32 palette[16][4];
  for (u32 k = 0; k < 16; ++k) {
    const u32 w = kWeight4[k];
    for (int c = 0; c < 4; ++c) {
      palette[k][c] = static_cast<f32>(
          ((64 - w) * static_cast<u32>(e0[c]) + w * static_cast<u32>(e1[c]) + 32) >> 6);
    }
  }
  f32 dir[4];
  f32 dir_len2 = 0;
  for (int c = 0; c < 4; ++c) {
    dir[c] = static_cast<f32>(e1[c]) - static_cast<f32>(e0[c]);
    dir_len2 += dir[c] * dir[c];
  }
  f32 total = 0;
  for (u32 i = 0; i < kBlockTexels; ++i) {
    // The ramp is near-uniform (the widest gap between kWeight4[k]/64 and
    // k/15 is under a tenth of a step), so the projected index plus one
    // neighbour either side is the exhaustive answer.
    u32 guess = 0;
    if (dir_len2 > 1e-6f) {
      f32 t = 0;
      for (int c = 0; c < 4; ++c) t += (points[i * 4 + c] - static_cast<f32>(e0[c])) * dir[c];
      t /= dir_len2;
      guess = static_cast<u32>(std::clamp(static_cast<int>(t * 15.0f + 0.5f), 0, 15));
    }
    const u32 first = guess > 0 ? guess - 1 : 0;
    const u32 last = std::min(15u, guess + 1);
    f32 best = 1e30f;
    u32 best_index = guess;
    for (u32 k = first; k <= last; ++k) {
      f32 err = 0;
      for (int c = 0; c < 4; ++c) {
        const f32 d = points[i * 4 + c] - palette[k][c];
        err += d * d;
      }
      if (err < best) {
        best = err;
        best_index = k;
      }
    }
    index[i] = static_cast<u8>(best_index);
    total += best;
  }
  return total;
}

struct BitWriter {
  u8* out;
  u32 bit = 0;
  void Put(u32 value, u32 count) {
    for (u32 i = 0; i < count; ++i) {
      if (value & (1u << i)) out[bit >> 3] |= static_cast<u8>(1u << (bit & 7u));
      ++bit;
    }
  }
};

}  // namespace

void EncodeBc1Block(const u8* rgba, u8* out) { EncodeColorBlock(rgba, out); }

void EncodeBc4Block(const u8* values, u8* out) {
  f32 v[kBlockTexels];
  for (u32 i = 0; i < kBlockTexels; ++i) v[i] = static_cast<f32>(values[i]);
  EncodeChannelBlock(v, out);
}

void EncodeBc3Block(const u8* rgba, u8* out) {
  f32 alpha[kBlockTexels];
  for (u32 i = 0; i < kBlockTexels; ++i) alpha[i] = static_cast<f32>(rgba[i * 4 + 3]);
  EncodeChannelBlock(alpha, out);
  EncodeColorBlock(rgba, out + 8);
}

void EncodeBc5Block(const u8* rgba, u8* out) {
  f32 channel[kBlockTexels];
  for (u32 i = 0; i < kBlockTexels; ++i) channel[i] = static_cast<f32>(rgba[i * 4 + 0]);
  EncodeChannelBlock(channel, out);
  for (u32 i = 0; i < kBlockTexels; ++i) channel[i] = static_cast<f32>(rgba[i * 4 + 1]);
  EncodeChannelBlock(channel, out + 8);
}

void EncodeBc7Block(const u8* rgba, u8* out) {
  f32 points[kBlockTexels * 4];
  for (u32 i = 0; i < kBlockTexels * 4; ++i) points[i] = static_cast<f32>(rgba[i]);

  f32 a[4];
  f32 b[4];
  FitLine<4>(points, a, b);

  // Refine the endpoints in float first, then spend the p-bit search on the
  // converged pair. The p-bit only moves an endpoint by one, so refining it
  // per parity buys almost nothing for four times the work.
  u8 q0[4];
  u8 q1[4];
  u8 index[kBlockTexels];
  for (int pass = 0; pass < 2; ++pass) {
    for (int c = 0; c < 4; ++c) {
      q0[c] = QuantizeWithParity(a[c], 0);
      q1[c] = QuantizeWithParity(b[c], 0);
    }
    AssignBc7Indices(points, q0, q1, index);
    f32 weights[kBlockTexels];
    for (u32 i = 0; i < kBlockTexels; ++i) {
      weights[i] = static_cast<f32>(kWeight4[index[i]]) * (1.0f / 64.0f);
    }
    RefitEndpoints<4>(points, weights, a, b);
  }

  u8 best0[4];
  u8 best1[4];
  u8 best_index[kBlockTexels];
  u32 best_p0 = 0;
  u32 best_p1 = 0;
  f32 best_error = 1e30f;
  for (u32 p0 = 0; p0 < 2; ++p0) {
    for (u32 p1 = 0; p1 < 2; ++p1) {
      for (int c = 0; c < 4; ++c) {
        q0[c] = QuantizeWithParity(a[c], p0);
        q1[c] = QuantizeWithParity(b[c], p1);
      }
      const f32 error = AssignBc7Indices(points, q0, q1, index);
      if (error >= best_error) continue;
      best_error = error;
      best_p0 = p0;
      best_p1 = p1;
      std::memcpy(best0, q0, sizeof(best0));
      std::memcpy(best1, q1, sizeof(best1));
      std::memcpy(best_index, index, sizeof(best_index));
    }
  }

  // The anchor texel's index carries no high bit (that is the bit the format
  // spends on the mode), so a block whose first texel sits past the middle of
  // the ramp has to be stored with its endpoints the other way round.
  if (best_index[0] > 7) {
    for (int c = 0; c < 4; ++c) std::swap(best0[c], best1[c]);
    std::swap(best_p0, best_p1);
    for (u32 i = 0; i < kBlockTexels; ++i) best_index[i] = static_cast<u8>(15 - best_index[i]);
  }

  std::memset(out, 0, 16);
  BitWriter writer{out};
  writer.Put(1u << 6, 7);  // mode 6: six zeros then a one
  for (int c = 0; c < 3; ++c) {
    writer.Put(static_cast<u32>(best0[c]) >> 1, 7);
    writer.Put(static_cast<u32>(best1[c]) >> 1, 7);
  }
  writer.Put(static_cast<u32>(best0[3]) >> 1, 7);
  writer.Put(static_cast<u32>(best1[3]) >> 1, 7);
  writer.Put(best_p0, 1);
  writer.Put(best_p1, 1);
  writer.Put(best_index[0], 3);
  for (u32 i = 1; i < kBlockTexels; ++i) writer.Put(best_index[i], 4);
}

bool DecodeBc7Block(const u8* block, u8* out_rgba) {
  // Mode 6 is six zero bits then a one, i.e. 0x40 in the low seven bits.
  if ((block[0] & 0x7fu) != 0x40u) return false;
  auto bits = [block](u32 offset, u32 count) -> u32 {
    u32 value = 0;
    for (u32 i = 0; i < count; ++i) {
      const u32 b = offset + i;
      value |= static_cast<u32>((block[b >> 3] >> (b & 7u)) & 1u) << i;
    }
    return value;
  };
  const u32 p0 = bits(63, 1);
  const u32 p1 = bits(64, 1);
  u32 e0[4];
  u32 e1[4];
  for (u32 c = 0; c < 4; ++c) {
    e0[c] = (bits(7 + c * 14, 7) << 1) | p0;
    e1[c] = (bits(14 + c * 14, 7) << 1) | p1;
  }
  u32 offset = 65;
  for (u32 t = 0; t < kBlockTexels; ++t) {
    const u32 count = t == 0 ? 3u : 4u;
    const u32 w = kWeight4[bits(offset, count)];
    offset += count;
    for (u32 c = 0; c < 4; ++c) {
      out_rgba[t * 4 + c] = static_cast<u8>(((64 - w) * e0[c] + w * e1[c] + 32) >> 6);
    }
  }
  return true;
}

}  // namespace rx::asset
