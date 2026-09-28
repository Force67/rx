#ifndef RX_SHADERS_AURORA_HLSLI
#define RX_SHADERS_AURORA_HLSLI

// Aurora borealis, shared between the screen-space sky background (sky.ps)
// and the sky cubemap generator (sky.cs) so the same curtains land in the IBL.
//
// Auroral emission lives in a thin spherical shell roughly 95-260 km up:
// precipitating electrons excite the upper atmosphere along near-vertical
// magnetic field lines, so the light organises into tall curtains that follow
// long curved arcs across the sky. We raymarch that shell between its two dome
// radii and evaluate a few independent curtain systems per sample: each system
// is a meandering path in the horizontal plane (a planetary-scale sine warped
// by drifting fbm), a Gaussian falloff across the path making the sheet, and a
// high-frequency striation term along the path making the thin vertical rays.
// Colour follows the real emission ladder: 557.7 nm atomic-oxygen green
// dominates the dense lower edge, blending through teal into the faint 630 nm
// red / N2+ purple fringe where the air is too thin to quench the slow upper
// transitions. The lower border is sharp (collisions kill the emission below
// ~95 km); the top diffuses out with the electron energy spectrum.

#include "atmosphere.hlsli"

static const float kAuroraBottom = 95e3;   // sharp lower curtain edge (m)
static const float kAuroraTop = 260e3;     // diffuse upper fringe (m)

float AuroraHash(float2 p) {
  p = frac(p * float2(233.34, 851.73));
  p += dot(p, p + 23.45);
  return frac(p.x * p.y);
}
float AuroraNoise(float2 p) {
  float2 i = floor(p), f = frac(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = AuroraHash(i), b = AuroraHash(i + float2(1, 0));
  float c = AuroraHash(i + float2(0, 1)), d = AuroraHash(i + float2(1, 1));
  return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
float AuroraFbm(float2 p) {
  float s = 0.0, a = 0.5;
  for (int i = 0; i < 4; ++i) { s += a * AuroraNoise(p); p = p * 2.03 + 17.1; a *= 0.5; }
  return s;
}
float2 AuroraRot(float2 p, float a) {
  float c = cos(a), s = sin(a);
  return float2(c * p.x - s * p.y, s * p.x + c * p.y);
}

// Density of one curtain system at a horizontal point q (km, already rotated
// into the system's frame) and normalised shell altitude u. The rest line
// meanders with a slow sine plus an fbm warp that drifts over minutes, so the
// arc curves across the sky instead of running axis-aligned.
float AuroraCurtain(float2 q, float u, float t, float seed) {
  float path = sin(q.x * 0.011 + seed * 5.0 + t * 0.7) * 55.0 +
               (AuroraFbm(float2(q.x * 0.006 + seed, t * 0.15 + seed * 3.0)) - 0.5) * 180.0;
  float d = q.y - path;
  // The sheet: a few-km bright core that broadens as it climbs, matching the
  // field-aligned electron beams spreading out with altitude.
  float width = 9.0 + u * 26.0;
  float sheet = exp(-d * d / (width * width));
  // Striations: high-frequency noise along the arc, effectively stretched
  // along altitude (it barely depends on u) and shimmering slowly. Two octaves
  // beat against each other so rays split and merge rather than scroll.
  float s = q.x * 0.55 + d * 0.08;
  float rays = AuroraNoise(float2(s, t * 1.7 + seed * 9.0)) * 0.65 +
               AuroraNoise(float2(s * 3.1 + 4.7, seed * 7.0 - t * 1.1)) * 0.35;
  rays = pow(saturate(rays * 1.35 - 0.18), 3.0);
  return sheet * (0.25 + 1.5 * rays);
}

// Emitted curtain radiance along a world-space view direction. `intensity` 1
// is a strong display; 0 skips all work. Callers gate on their own night
// factor so the curtains never fight the daylight sky.
float3 AuroraRadiance(float3 dir, float time, float intensity) {
  if (intensity <= 0.0 || dir.y < 0.02) return 0.0.xxx;
  // March between the two shell domes, eye near the ground like the sky pass.
  float3 p0 = float3(0.0, kGroundRadius + 500.0, 0.0);
  float t_in = RaySphere(p0, dir, kGroundRadius + kAuroraBottom);
  float t_out = RaySphere(p0, dir, kGroundRadius + kAuroraTop);
  if (t_in < 0.0 || t_out <= t_in) return 0.0.xxx;

  const int kAuroraSteps = 22;
  float t = time * 0.03;  // majestic: a full undulation takes minutes
  // A static per-direction jitter of the sample comb: oblique rays cross the
  // shell in strides wider than a curtain core, and the dither trades the
  // resulting banding for unstructured noise.
  float jitter = AuroraHash(dir.xz * 431.7 + dir.y * 917.3);
  float dt = (t_out - t_in) / float(kAuroraSteps);

  float3 radiance = 0.0.xxx;
  for (int i = 0; i < kAuroraSteps; ++i) {
    float3 pos = p0 + dir * (t_in + (float(i) + jitter) * dt);
    float u = saturate((length(pos) - kGroundRadius - kAuroraBottom) /
                       (kAuroraTop - kAuroraBottom));
    float2 xz = pos.xz * 1e-3;  // horizontal footprint in km

    // Three independent systems at unrelated orientations and phases, the
    // secondary arcs fainter, as in a real multi-band display.
    float density = AuroraCurtain(AuroraRot(xz, 0.3), u, t, 0.0) +
                    AuroraCurtain(AuroraRot(xz + 400.0, 2.4), u, t + 30.0, 1.0) * 0.7 +
                    AuroraCurtain(AuroraRot(xz - 700.0, 4.3), u, t + 70.0, 2.0) * 0.5;

    // Vertical emission profile: a sharp collisional cutoff at the bottom
    // edge, then an exponential falloff as the shell thins.
    float profile = smoothstep(0.0, 0.045, u) * exp(-u * 3.2);

    // Green floor -> teal mid -> purple/red fringe up the emission ladder.
    float3 c = lerp(float3(0.10, 1.00, 0.35), float3(0.10, 0.75, 0.65),
                    smoothstep(0.02, 0.35, u));
    c = lerp(c, float3(0.60, 0.20, 0.90), smoothstep(0.30, 0.85, u));

    radiance += c * (density * profile);
  }
  // Normalise by step count (not path length) so the near-horizon slant does
  // not blow out, then fade the last degrees above the horizon where the real
  // thing is buried in airmass extinction.
  radiance /= float(kAuroraSteps);
  float horizon = smoothstep(0.02, 0.10, dir.y);
  radiance *= horizon * intensity * 1.5;
  // A hue-preserving luminance shoulder: where a curtain is seen edge-on many
  // samples stack in its dense core, and without compression that column
  // clips to white under night auto-exposure. The low end passes untouched,
  // so the curtains still read as luminous.
  float lum = dot(radiance, float3(0.2126, 0.7152, 0.0722));
  return radiance / (1.0 + 0.35 * lum);
}

#endif  // RX_SHADERS_AURORA_HLSLI
