#ifndef RX_CLOUDSCAPE_NOISE_HLSLI_
#define RX_CLOUDSCAPE_NOISE_HLSLI_

// Shared procedural-noise primitives for the cloudscape texture bakes. Every
// generator here is *tileable*: the shape/erosion volumes and the weather map
// are sampled with wrap addressing across an unbounded sky, so a visible seam
// at the texture wrap would tile across the whole horizon. Seamlessness is not
// cosmetic here, it is the acceptance bar, and it is achieved the only way that
// survives fbm: the lattice coordinate feeding every gradient / feature-point
// hash is wrapped modulo the octave's period before it is hashed. Two lattice
// cells one period apart then hash to the same value, so the field is exactly
// periodic with period 1.0 in the [0,1) sampling domain at every octave.

float cs_remap(float v, float l0, float h0, float l1, float h1) {
  return l1 + (v - l0) * (h1 - l1) / (h0 - l0);
}

// Positive modulo (HLSL fmod keeps the sign of the dividend, which would break
// the wrap for the negative neighbour cells a Worley search visits).
float3 cs_mod3(float3 p, float m) { return p - m * floor(p / m); }
float2 cs_mod2(float2 p, float m) { return p - m * floor(p / m); }

// Hash of an (already wrapped) integer lattice coordinate. Deterministic for
// identical inputs, which is what makes the wrap tile: the two edges of the
// period feed bit-identical coordinates and so read back identical gradients.
float3 cs_hash33(float3 p) {
  p = float3(dot(p, float3(127.1, 311.7, 74.7)),
             dot(p, float3(269.5, 183.3, 246.1)),
             dot(p, float3(113.5, 271.9, 124.6)));
  return frac(sin(p) * 43758.5453123);
}
float2 cs_hash22(float2 p) {
  p = float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)));
  return frac(sin(p) * 43758.5453123);
}

// --- tileable 3D Perlin (gradient) noise, period = freq cells ---
float cs_perlin3(float3 x, float freq) {
  float3 p = x * freq;
  float3 pi = floor(p);
  float3 pf = p - pi;
  float3 u = pf * pf * pf * (pf * (pf * 6.0 - 15.0) + 10.0);  // quintic fade
  float sum = 0.0;
  [unroll]
  for (int cz = 0; cz <= 1; ++cz)
    [unroll]
    for (int cy = 0; cy <= 1; ++cy)
      [unroll]
      for (int cx = 0; cx <= 1; ++cx) {
        float3 c = float3(cx, cy, cz);
        float3 g = cs_hash33(cs_mod3(pi + c, freq)) * 2.0 - 1.0;  // wrapped lattice
        float w = lerp(1.0 - u.x, u.x, c.x) * lerp(1.0 - u.y, u.y, c.y) *
                  lerp(1.0 - u.z, u.z, c.z);
        sum += w * dot(g, pf - c);
      }
  return sum;  // ~[-1, 1]
}

float cs_perlin3_fbm(float3 p, float base, int octaves) {
  float sum = 0.0, amp = 0.5, norm = 0.0, freq = base;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * cs_perlin3(p, freq);
    norm += amp;
    freq *= 2.0;
    amp *= 0.5;
  }
  return saturate(sum / norm * 0.5 + 0.5);  // [0, 1]
}

// --- tileable 3D Worley (cellular) F1 distance, period = freq cells ---
float cs_worley3(float3 x, float freq) {
  float3 p = x * freq;
  float3 pi = floor(p);
  float3 pf = p - pi;
  float mind = 1e9;
  [unroll]
  for (int cz = -1; cz <= 1; ++cz)
    [unroll]
    for (int cy = -1; cy <= 1; ++cy)
      [unroll]
      for (int cx = -1; cx <= 1; ++cx) {
        float3 off = float3(cx, cy, cz);
        float3 fp = off + cs_hash33(cs_mod3(pi + off, freq));  // wrapped cell point
        float3 d = fp - pf;
        mind = min(mind, dot(d, d));
      }
  return sqrt(mind);  // ~[0, ~1.2]
}

// Inverted-Worley fbm: three octaves doubling in frequency, weighted so the
// coarse cells dominate. Inverted (1 - F1) so feature points read bright, which
// gives the billowing cauliflower silhouette the raymarcher erodes with.
float cs_worley3_fbm(float3 p, float base) {
  float w0 = 1.0 - saturate(cs_worley3(p, base));
  float w1 = 1.0 - saturate(cs_worley3(p, base * 2.0));
  float w2 = 1.0 - saturate(cs_worley3(p, base * 4.0));
  return saturate(0.625 * w0 + 0.25 * w1 + 0.125 * w2);
}

// --- tileable 2D Perlin + inverted-Worley fbm (weather map + curl potential) ---
float cs_perlin2(float2 x, float freq) {
  float2 p = x * freq;
  float2 pi = floor(p);
  float2 pf = p - pi;
  float2 u = pf * pf * pf * (pf * (pf * 6.0 - 15.0) + 10.0);
  float sum = 0.0;
  [unroll]
  for (int cy = 0; cy <= 1; ++cy)
    [unroll]
    for (int cx = 0; cx <= 1; ++cx) {
      float2 c = float2(cx, cy);
      float2 g = cs_hash22(cs_mod2(pi + c, freq)) * 2.0 - 1.0;
      float w = lerp(1.0 - u.x, u.x, c.x) * lerp(1.0 - u.y, u.y, c.y);
      sum += w * dot(g, pf - c);
    }
  return sum;
}

float cs_perlin2_fbm(float2 p, float base, int octaves) {
  float sum = 0.0, amp = 0.5, norm = 0.0, freq = base;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * cs_perlin2(p, freq);
    norm += amp;
    freq *= 2.0;
    amp *= 0.5;
  }
  return saturate(sum / norm * 0.5 + 0.5);
}

float cs_worley2(float2 x, float freq) {
  float2 p = x * freq;
  float2 pi = floor(p);
  float2 pf = p - pi;
  float mind = 1e9;
  [unroll]
  for (int cy = -1; cy <= 1; ++cy)
    [unroll]
    for (int cx = -1; cx <= 1; ++cx) {
      float2 off = float2(cx, cy);
      float2 fp = off + cs_hash22(cs_mod2(pi + off, freq));
      float2 d = fp - pf;
      mind = min(mind, dot(d, d));
    }
  return sqrt(mind);
}

float cs_worley2_fbm(float2 p, float base) {
  float w0 = 1.0 - saturate(cs_worley2(p, base));
  float w1 = 1.0 - saturate(cs_worley2(p, base * 2.0));
  float w2 = 1.0 - saturate(cs_worley2(p, base * 4.0));
  return saturate(0.625 * w0 + 0.25 * w1 + 0.125 * w2);
}

#endif  // RX_CLOUDSCAPE_NOISE_HLSLI_
