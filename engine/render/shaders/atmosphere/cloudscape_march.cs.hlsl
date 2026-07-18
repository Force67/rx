#include "rhi_bindings.hlsli"
// Cloudscape march: the opt-in textured cloud model. Runs at half resolution
// into a persistent buffer; each frame only 1 pixel of every 4x4 block marches
// fresh (a 16-frame refresh cycle), the rest reproject last frame's result
// through the stored cloud distance. The march itself is a two-mode walk
// through a spherical shell: cheap base-texture samples skip empty air, full
// samples (base + erosion + curl distortion) resolve the cloud interior.
// Lighting is a 6-tap cone toward the sun with Beer extinction, a two-lobe
// phase, a multiple-scatter floor and a powder term; precipitating cells
// absorb harder so storm decks go dark-bottomed.

#include "atmosphere.hlsli"

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_cloud : register(u0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> out_dist : register(u1, space0);
[[vk::binding(2, 0)]] Texture2D<float4> prev_cloud : register(t2, space0);
[[vk::binding(3, 0)]] Texture2D<float> prev_dist : register(t3, space0);
[[vk::binding(4, 0)]] Texture2D<float> depth_in : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture3D<float4> base_noise : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState base_sampler : register(s5, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] Texture3D<float4> detail_noise : register(t6, space0);
[[vk::combinedImageSampler]] [[vk::binding(6, 0)]] SamplerState detail_sampler : register(s6, space0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]] Texture2D<float2> curl_noise : register(t7, space0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]] SamplerState curl_sampler : register(s7, space0);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] Texture2D<float4> weather_map : register(t8, space0);
[[vk::combinedImageSampler]] [[vk::binding(8, 0)]] SamplerState weather_sampler : register(s8, space0);

struct PushData {
  column_major float4x4 inv_view_proj;
  column_major float4x4 prev_view_proj;
  float4 camera_pos;     // xyz eye (m), w time (s)
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w ambient scale
  float4 wind;           // xy blow direction (unit XZ), z speed m/s, w vertical skew m
  float4 shape;          // x bottom(m), y top(m), z density, w turbulence
  float4 map;            // xy map offset (m), z map extent (m), w anvil
  uint2 size;            // half-res buffer size
  uint2 full_size;       // scene color/depth size
  uint frame_index;
  uint steps;            // potential full samples toward the zenith
  uint flags;            // bit 0: history valid
  float darkness;        // menace 0..1: blackens the deck (severe-storm skies)
};
[[vk::binding(9, 0)]] ConstantBuffer<PushData> pc : register(b9, space0);

static const float kBaseScale = 1.0 / 9600.0;    // base noise tiles every ~9.6 km
static const float kDetailScale = 1.0 / 1100.0;  // erosion texture tile
static const float kCurlScale = 1.0 / 5200.0;

float Remap(float v, float l0, float h0, float l1, float h1) {
  return l1 + (saturate((v - l0) / max(h0 - l0, 1e-5)) * (h1 - l1));
}

// Vertical profiles for the three low-altitude cloud classes. cloud_type
// blends stratus -> stratocumulus -> cumulus over 0..1; the profile gates
// where density may exist over the shell height, which is what makes the
// noise read as a recognizable cloud class instead of a uniform fog slab.
float HeightProfile(float h, float cloud_type, float anvil) {
  float stratus = Remap(h, 0.0, 0.06, 0.0, 1.0) * Remap(h, 0.18, 0.28, 1.0, 0.0);
  float strato = Remap(h, 0.02, 0.16, 0.0, 1.0) * Remap(h, 0.45, 0.62, 1.0, 0.0);
  float cumulus = Remap(h, 0.0, 0.12, 0.0, 1.0) * Remap(h, 0.68, 0.95, 1.0, 0.0);
  float profile = cloud_type < 0.5 ? lerp(stratus, strato, cloud_type * 2.0)
                                   : lerp(strato, cumulus, cloud_type * 2.0 - 1.0);
  // Storminess lets density survive to the shell top and spread there: the
  // classic flat-topped anvil silhouette.
  float top = Remap(h, 0.6, 0.95, 0.0, 1.0);
  return lerp(profile, max(profile, top * 0.7), anvil * saturate(cloud_type * 2.0));
}

// Weather map: R coverage, G precipitation, B cloud type. World XZ, wrapped.
float4 SampleWeather(float2 world_xz) {
  float2 uv = (world_xz - pc.map.xy) / pc.map.z;
  return weather_map.SampleLevel(weather_sampler, uv, 0.0);
}

float HeightIn(float3 p) {
  return (length(p) - (kGroundRadius + pc.shape.x)) / max(pc.shape.y - pc.shape.x, 1.0);
}

// The march runs in the planet frame (camera on the +y axis, so p.xz is the
// XZ offset from the eye). Every texture lookup must be world-anchored or the
// whole deck would ride along with the camera: unwrap the shell into world
// coordinates -- world XZ by adding the eye, altitude along the radial.
float3 WorldUnwrap(float3 p) {
  return float3(p.x + pc.camera_pos.x, length(p) - kGroundRadius, p.z + pc.camera_pos.z);
}

// Wind advection: the whole field drifts downwind, and the layer top leads the
// base (vertical skew) so towers lean the way real shear pushes them.
float3 Advect(float3 wp, float h) {
  float2 drift = pc.map.xy + pc.wind.xy * (pc.wind.w * h);
  return wp - float3(drift.x, 0.0, drift.y);
}

// Cheap density: base texture, profile and coverage only. Used to skip empty
// air and for the far light-cone taps; no erosion work.
// Fraction of the shell thickness the virga skirt hangs below the base.
static const float kVirgaDepth = 0.22;

// Virga: rain shafts hanging under precipitating cells, evaporating before
// (or at) the ground. Sampled for h < 0 -- vertically stretched noise gives
// the streaked curtain look, fading toward the shaft's ragged bottom.
float VirgaDensity(float3 p, float h, float4 weather) {
  if (h < -kVirgaDepth) return 0.0;
  float gate = saturate(weather.g * 1.7 - 0.18);
  if (gate <= 0.0) return 0.0;
  float3 wp = Advect(WorldUnwrap(p), 0.0);
  // Compress y hard so the noise reads as vertical streaks, not blobs.
  float3 sp = float3(wp.x, wp.y * 0.14, wp.z) * (kBaseScale * 2.4);
  float n = base_noise.SampleLevel(base_sampler, sp, 0.0).g;
  float fade = 1.0 + h / kVirgaDepth;  // 1 at the base, 0 at the skirt bottom
  return gate * saturate(n - 0.42) * 1.6 * fade * fade * 0.10;
}

float DensityCheap(float3 p, float4 weather) {
  float h = HeightIn(p);
  if (h < 0.0) return VirgaDensity(p, h, weather);
  if (h >= 1.0) return 0.0;
  float coverage = weather.r;
  if (coverage <= 0.005) return 0.0;
  float3 sp = Advect(WorldUnwrap(p), h) * kBaseScale;
  float4 nse = base_noise.SampleLevel(base_sampler, sp, 0.0);
  float worley_fbm = nse.g * 0.625 + nse.b * 0.25 + nse.a * 0.125;
  float base = Remap(nse.r, worley_fbm - 1.0, 1.0, 0.0, 1.0);
  base *= HeightProfile(h, weather.b, pc.map.w * weather.g);
  // Coverage as a remap, not a fade: raising it makes more of the noise field
  // cross the threshold, so formations grow and join instead of ghosting in.
  float d = Remap(base, 1.0 - coverage, 1.0, 0.0, 1.0) * coverage;
  return d;
}

// Full density: cheap shape eroded by the detail texture, whose lookup is
// swirled by curl noise. Erosion works on the boundary (scaled by 1-d) so the
// large silhouette survives while edges shred into wisps; near the base the
// detail flips (1-fbm) for the thin foggy underside.
float DensityFull(float3 p, float4 weather) {
  float h = HeightIn(p);
  if (h < 0.0) return VirgaDensity(p, h, weather);  // shafts need no erosion
  if (h >= 1.0) return 0.0;
  float d = DensityCheap(p, weather);
  if (d <= 0.0) return 0.0;
  float3 ap = Advect(WorldUnwrap(p), h);
  float2 curl = curl_noise.SampleLevel(curl_sampler, ap.xz * kCurlScale, 0.0).xy;
  float3 dp = ap * kDetailScale;
  dp.xz += curl * pc.shape.w * (1.0 - h) * 0.55;
  float3 det = detail_noise.SampleLevel(detail_sampler, dp, 0.0).rgb;
  float det_fbm = det.r * 0.625 + det.g * 0.25 + det.b * 0.125;
  float modifier = lerp(1.0 - det_fbm, det_fbm, saturate(h * 8.0));
  // Edge-only erosion: full strength where the base density is thin, none in
  // the core, so the detail texture's own tile never reads across the sky.
  float erosion = modifier * 0.45 * (1.0 - 0.7 * saturate(d * 2.0));
  d = Remap(d, erosion, 1.0, 0.0, 1.0);
  return d * pc.shape.z;
}

float HG(float c, float g) {
  float g2 = g * g;
  return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// 6-tap light cone toward the sun: 5 jittered near taps inside a widening cone
// (nearby density softens the estimate instead of banding) plus one far tap so
// distant towers still throw shadows across the deck.
float LightEnergy(float3 p, float3 to_sun, float4 weather, float cos_angle, float powder_depth) {
  static const float3 kCone[5] = {
    float3(0.30, 0.00, 0.16), float3(-0.25, 0.12, -0.20), float3(0.10, -0.28, 0.24),
    float3(-0.14, 0.26, 0.10), float3(0.22, 0.18, -0.28)
  };
  float step_len = (pc.shape.y - pc.shape.x) * 0.09;
  float optical = 0.0;
  [unroll]
  for (int i = 0; i < 5; ++i) {
    float dist = (float(i) + 0.6) * step_len;
    float3 sp = p + (to_sun + kCone[i] * (float(i) * 0.35)) * dist;
    // Near taps use the full model (edge shadowing needs the eroded shape),
    // far taps the cheap one.
    float2 sxz = sp.xz + pc.camera_pos.xz;
    float ds = i < 3 ? DensityFull(sp, SampleWeather(sxz)) : DensityCheap(sp, SampleWeather(sxz));
    optical += ds * step_len;
  }
  float3 fp = p + to_sun * (step_len * 12.0);
  optical += DensityCheap(fp, SampleWeather(fp.xz + pc.camera_pos.xz)) * step_len * 3.0;

  // Precipitating cells absorb harder: rain cores go graphite instead of
  // white. Kept moderate -- a deck that clamps to black under rain stops
  // reading as cloud at all. Menace stacks on top for the authored version,
  // gated by the LOCAL precip cell: a distant storm blackens its own corner
  // of the sky while the deck over the camera keeps its daylight.
  float dark = pc.darkness * saturate(weather.g * 1.8);
  float absorb = 0.035 * (1.0 + weather.g * 1.1) * (1.0 + dark * 2.0);
  // Three-scale Beer floor stands in for multiple scattering: even a deep
  // overcast deck keeps the bright diffuse glow a real one transmits. Rain
  // RAISES the widest floor (a soaked deck scatters more, which is what keeps
  // it legible against the precipitation haze); menace collapses all of it.
  float floor_gain = (1.0 - 0.65 * dark);
  float beer = max(max(exp(-optical * absorb), exp(-optical * absorb * 0.2) * 0.28 * floor_gain),
                   exp(-optical * absorb * 0.06) * 0.14 * (1.0 + weather.g * 0.9) * floor_gain);
  // Powder: freshly entered thin edges under-collect in-scattered light, so
  // sugary detail survives where beer alone would flatten it.
  float powder = 1.0 - exp(-powder_depth * 2.0);
  float phase = lerp(HG(cos_angle, 0.55), HG(cos_angle, -0.18), 0.42);
  return beer * lerp(0.25, 1.0, powder) * phase;
}

float Ign(uint2 px, uint frame) {
  float3 m = float3(0.06711056, 0.00583715, 52.9829189);
  return frac(m.z * frac(dot(float2(px) + float(frame & 31u) * 5.588, m.xy)));
}

// 4x4 ordered refresh: pixel (x,y) of each block re-marches on this frame of
// the 16-frame cycle. Bayer order spreads consecutive refreshes apart so a
// half-updated block never reads as a sliding seam.
static const uint kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

struct MarchResult {
  float3 scatter;
  float transmittance;
  float dist;  // transmittance-weighted mean cloud distance, <0 = occluded
};

MarchResult March(float3 cam, float3 view, float scene_dist, uint2 px) {
  MarchResult r;
  r.scatter = 0.0.xxx;
  r.transmittance = 1.0;
  r.dist = -1.0;

  // Shell geometry in the planet frame; only the camera altitude matters.
  float3 p0 = float3(0.0, kGroundRadius + max(cam.y, 0.0) + 1.0, 0.0);
  // The marched interval reaches below the cloud base by the virga depth so
  // rain shafts under precipitating cells are inside it; the density function
  // returns 0 there when nothing precipitates and the cheap stride skips it.
  float Rb = kGroundRadius +
             max(pc.shape.x - (pc.shape.y - pc.shape.x) * 0.22, 60.0);
  float Rt = kGroundRadius + pc.shape.y;
  float rc = length(p0);
  float t_start, t_end;
  if (rc <= Rb) {
    t_start = RaySphere(p0, view, Rb);
    t_end = RaySphere(p0, view, Rt);
  } else if (rc >= Rt) {
    t_start = RaySphere(p0, view, Rt);
    float tb = RaySphere(p0, view, Rb);
    t_end = tb > 0.0 ? tb : t_start;
  } else {
    t_start = 0.0;
    t_end = RaySphere(p0, view, Rt);
    float tb = RaySphere(p0, view, Rb);
    if (tb > 0.0) t_end = min(t_end, tb);
  }
  bool occluded = scene_dist < t_start;
  bool geometry_clipped_shell = scene_dist < t_end;
  t_end = min(t_end, scene_dist);
  if (t_start < 0.0 || t_end <= t_start || t_start > 180000.0) {
    // No shell interval: valid "no cloud" unless geometry hid the sky.
    r.dist = occluded ? -1.0 : (t_start > 0.0 ? t_start : 1e7);
    return r;
  }

  float3 to_sun = normalize(-pc.sun_direction.xyz);
  float3 sun_col = pc.sun_color.rgb * pc.sun_direction.w;
  // The ambient the deck bathes in follows the sky: cool blue under a high
  // sun, and as the sun drops toward the horizon it hands over to the warm
  // band of a low atmosphere -- multiplied by the sun colour (which the clock
  // already reddens at dusk), this is what sets evening decks on fire instead
  // of leaving them grey-blue at sunset.
  float sun_elev = saturate(to_sun.y * 2.2 + 0.08);
  float3 sky_tint = lerp(float3(1.0, 0.42, 0.26), float3(0.50, 0.62, 0.88), sun_elev);
  float3 warm_sun = lerp(pc.sun_color.rgb, float3(1, 1, 1), 0.35);  // keep some neutral fill
  float3 ambient_base = sky_tint * warm_sun * 0.5 * pc.sun_direction.w * pc.sun_color.w;
  float cos_angle = dot(view, to_sun);

  // More potential samples toward the horizon, where the slab is thickest.
  uint steps = max(pc.steps, 8u);
  steps = uint(lerp(float(steps) * 2.0, float(steps), abs(view.y)));
  float dt_full = (t_end - t_start) / float(steps);
  // The march can't afford full-detail samples in empty air: cheap base-only
  // taps stride 3x wider until they hit possible cloud, then the walk backs up
  // one stride and drops to full samples until the cloud is left again.
  float dt_cheap = dt_full * 3.0;
  // Static per-pixel jitter: neighbours stay decorrelated (no banding) but a
  // pixel re-marches at the same phase every cycle, so the history converges.
  float t = t_start + Ign(px, 0u) * dt_cheap;
  float weighted_t = 0.0;
  float weight_sum = 0.0;
  bool full_mode = false;
  uint empty_run = 0;

  [loop]
  while (t < t_end && r.transmittance > 0.01) {
    float3 pos = p0 + view * t;
    float2 world_xz = cam.xz + view.xz * t;
    float4 weather = SampleWeather(world_xz);

    if (!full_mode) {
      if (DensityCheap(pos, weather) <= 0.0) {
        t += dt_cheap;
        continue;
      }
      // Possible cloud: back up so the boundary isn't skipped, go fine.
      t = max(t - dt_cheap, t_start);
      full_mode = true;
      empty_run = 0;
      continue;
    }

    float density = DensityFull(pos, weather);
    if (density <= 0.001) {
      if (++empty_run >= 6) full_mode = false;  // left the cloud: stride out
      t += dt_full;
      continue;
    }
    empty_run = 0;

    float h = saturate(HeightIn(pos));
    float powder_depth = density * dt_full * 0.011;
    float light = LightEnergy(pos, to_sun, weather, cos_angle, powder_depth);
    // Away from the sun the ambient term carries the whole image, so it has
    // to vary or the deck flattens into a painted ceiling. Two cheap taps up
    // the radial estimate the cloud mass overhead: a sample under a heavy
    // column loses its sky dome and goes dark, a sample under a thin spot
    // glows -- exactly the mottled underside a real deck shows from below.
    float3 up = pos / max(length(pos), 1.0);
    float up_od = DensityCheap(pos + up * 350.0, weather) * 350.0 +
                  DensityCheap(pos + up * 950.0, weather) * 650.0;
    // Scaled like the sun march's absorption: typical overhead columns land
    // mid-curve, so the mottle actually varies instead of saturating.
    float sky_vis = exp(-up_od * 0.004);
    float3 ambient = ambient_base * (0.35 + 0.65 * h) * (1.0 - 0.28 * weather.g) *
                     (0.2 + 0.8 * sky_vis) *
                     (1.0 - 0.8 * pc.darkness * saturate(weather.g * 1.8));
    float3 lit = sun_col * light + ambient * 0.3;

    // Rain thickens the view extinction: soaked decks cut harder silhouettes
    // against the haze instead of dissolving into it. The base coefficient
    // stays permeable enough that rays reach a little interior before
    // saturating -- an opaque skin flattens every deck into one grey sheet.
    float sigma = density * 0.045 * (1.0 + weather.g * 0.6);
    float step_trans = exp(-sigma * dt_full);
    float contrib = r.transmittance * (1.0 - step_trans);
    r.scatter += contrib * lit;
    weighted_t += contrib * t;
    weight_sum += contrib;
    r.transmittance *= step_trans;
    t += dt_full;
  }

  // High wisp sheet above the shell: one stretched-fbm tap, nearly free, keeps
  // the upper sky from reading empty on clear days.
  if (r.transmittance > 0.02) {
    float t_ci = RaySphere(p0, view, kGroundRadius + pc.shape.y + 4200.0);
    if (t_ci > 0.0 && t_ci < min(scene_dist, 260000.0)) {
      float2 cxz = cam.xz + view.xz * t_ci;
      float4 wci = SampleWeather(cxz);
      float2 cuv = (cxz - pc.map.xy) * 0.000045;
      float3 cp = float3(cuv.x * 2.6, cuv.y * 1.2, 3.1);
      float wisp = base_noise.SampleLevel(base_sampler, cp * 0.13, 0.0).g * 0.7 +
                   base_noise.SampleLevel(base_sampler, cp * 0.31, 0.0).b * 0.3;
      float ci = saturate(wisp - (0.82 - wci.r * 0.22)) * 1.2 * saturate(wci.r * 3.0);
      if (ci > 0.001) {
        float ci_phase = max(HG(cos_angle, 0.55), 0.4 * HG(cos_angle, -0.1));
        float3 ci_lit = sun_col * ci_phase * 0.9 + ambient_base * 0.8;
        float ci_opacity = r.transmittance * (1.0 - exp(-ci * 0.3));
        r.scatter += r.transmittance * ci_lit * ci * 0.14;
        weighted_t += ci_opacity * t_ci;
        weight_sum += ci_opacity;
        r.transmittance *= exp(-ci * 0.3);
      }
    }
  }

  if (weight_sum > 1e-4) {
    r.dist = weighted_t / weight_sum;
  } else {
    r.dist = (occluded || geometry_clipped_shell) ? -1.0 : (t_start + t_end) * 0.5;
  }
  return r;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  uint2 px = id.xy;

  float2 ndc = (float2(px) + 0.5) / float2(pc.size) * 2.0 - 1.0;
  float4 nh = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));  // reversed-z near
  float3 cam = pc.camera_pos.xyz;
  float3 view = normalize(nh.xyz / nh.w - cam);

  // Scene distance from the full-res depth (point tap at our center).
  uint2 fpx = min(uint2((float2(px) + 0.5) * float2(pc.full_size) / float2(pc.size)),
                  pc.full_size - 1);
  float depth = depth_in.Load(int3(fpx, 0));
  float scene_dist = 1e12;
  if (depth > 0.0) {
    float2 depth_ndc = (float2(fpx) + 0.5) / float2(pc.full_size) * 2.0 - 1.0;
    float4 wh = mul(pc.inv_view_proj, float4(depth_ndc, depth, 1.0));
    scene_dist = length(wh.xyz / wh.w - cam);
  }

  bool history_valid = (pc.flags & 1u) != 0u;
  bool fresh = kBayer[(px.y & 3u) * 4u + (px.x & 3u)] == (pc.frame_index & 15u);

  // Reproject through the stored cloud distance: guess with a far point, read
  // the neighbourhood's marched distance, then re-project the actual world
  // point. Two projections make camera translation parallax-correct at cloud
  // range. hist_dist <= 0 = no usable history (off-screen or disoccluded).
  float4 hist_cloud = 0.0.xxxx;
  float hist_dist = -1.0;
  // Wind advection between frames is sub-texel at cloud distances, so the
  // reprojection deliberately ignores it; the refresh cycle and the apply
  // pass's tent filter absorb the residual. (A per-frame drift correction was
  // tried and creates a feedback smear worse than the error it removes.)
  if (history_valid) {
    float4 far_h = mul(pc.prev_view_proj, float4(cam + view * 30000.0, 1.0));
    if (far_h.w > 0.0) {
      // Engine convention: uv = ndc * 0.5 + 0.5, no y-flip (the projection
      // already carries the vulkan flip; recon_temporal.cs does the same).
      float2 far_uv = far_h.xy / far_h.w * 0.5 + 0.5;
      if (all(far_uv >= 0.0) && all(far_uv <= 1.0)) {
        float t_prev = prev_dist.Load(int3(min(uint2(far_uv * float2(pc.size)), pc.size - 1), 0));
        if (t_prev > 0.0) {
          float4 ph = mul(pc.prev_view_proj, float4(cam + view * t_prev, 1.0));
          if (ph.w > 0.0) {
            float2 puv = ph.xy / ph.w * 0.5 + 0.5;
            if (all(puv >= 0.0) && all(puv <= 1.0)) {
              uint2 ppx = min(uint2(puv * float2(pc.size)), pc.size - 1);
              float d_hist = prev_dist.Load(int3(ppx, 0));
              if (d_hist > 0.0) {
                hist_cloud = prev_cloud.Load(int3(ppx, 0));
                hist_dist = d_hist;
              }
            }
          }
        }
      }
    }
  }

  // Current geometry wins over history generated when this ray still saw sky.
  if (hist_dist > scene_dist) hist_dist = -1.0;

  if (!fresh && hist_dist > 0.0) {
    out_cloud[px] = hist_cloud;
    out_dist[px] = hist_dist;
    return;
  }

  MarchResult r = March(cam, view, scene_dist, px);
  // A refreshed pixel eases into the history rather than replacing it: the
  // per-pixel march phase differs from its neighbours', and a hard write
  // would pin that variance into the 4x4 refresh grid as visible dither.
  if (hist_dist > 0.0 && r.dist > 0.0) {
    out_cloud[px] = lerp(hist_cloud, float4(r.scatter, r.transmittance), 0.4);
    out_dist[px] = lerp(hist_dist, r.dist, 0.4);
  } else {
    out_cloud[px] = float4(r.scatter, r.transmittance);
    out_dist[px] = r.dist;
  }
}
