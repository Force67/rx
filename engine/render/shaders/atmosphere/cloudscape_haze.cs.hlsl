#include "rhi_bindings.hlsli"
// Cloudscape ground haze: an exponential height-fog layer that shares the
// deck's weather instead of being a flat screen effect. The optical depth is
// closed-form (no march) and then shaped by everything the clouds already
// know: the weather map makes humid cells fog up harder, two taps of the
// baked base noise drift mist banks with the wind, the in-scatter uses the
// same sun-elevation ambient the deck uses (so evening haze glows warm), the
// deck's coverage shades the fog beneath it, menace murks it, and the
// lightning flash breathes through it. Runs full-res after the cloud
// composite so the nearest scattering medium correctly veils both the scene
// and the distant deck.

#include "atmosphere.hlsli"

struct HazePush {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;     // xyz eye (m), w time (s)
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w flash 0..1
  float4 wind;           // xy blow direction, z vertical skew (m), w darkness
  float4 fog;            // x density 0..1, y falloff height (m), z ground (m), w anvil
  float4 map;            // xy map offset (m), z map extent (m), w churn 0..1
  uint2 size;
  float2 shell;          // deck bottom / top altitudes (m), for god-ray occlusion
};
[[vk::binding(6, 0)]] ConstantBuffer<HazePush> pc : register(b6, space0);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> color_in : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float> depth_in : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture3D<float4> base_noise : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState base_sampler : register(s3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2D<float4> weather_map : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState weather_sampler : register(s4, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2D<float4> transmittance_lut : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState transmittance_sampler : register(s5, space0);

static const float kBaseScale = 1.0 / 9600.0;  // matches cloudscape_march

float Remap(float v, float l0, float h0, float l1, float h1) {
  return l1 + (saturate((v - l0) / max(h0 - l0, 1e-5)) * (h1 - l1));
}

// Deck density for the god-ray occlusion taps, kept in lockstep with the
// march's cheap sampler (profile + coverage remap, no erosion).
float HeightProfile(float h, float cloud_type, float anvil) {
  float stratus = Remap(h, 0.0, 0.06, 0.0, 1.0) * Remap(h, 0.18, 0.28, 1.0, 0.0);
  float strato = Remap(h, 0.02, 0.16, 0.0, 1.0) * Remap(h, 0.45, 0.62, 1.0, 0.0);
  float cumulus = Remap(h, 0.0, 0.12, 0.0, 1.0) * Remap(h, 0.68, 0.95, 1.0, 0.0);
  float profile = cloud_type < 0.5 ? lerp(stratus, strato, cloud_type * 2.0)
                                   : lerp(strato, cumulus, cloud_type * 2.0 - 1.0);
  float top = Remap(h, 0.6, 0.95, 0.0, 1.0);
  return lerp(profile, max(profile, top * 0.7), anvil * saturate(cloud_type * 2.0));
}

float DeckDensity(float3 wp) {
  float h = (wp.y - pc.shell.x) / max(pc.shell.y - pc.shell.x, 1.0);
  if (h <= 0.0 || h >= 1.0) return 0.0;
  float4 weather = weather_map.SampleLevel(weather_sampler, (wp.xz - pc.map.xy) / pc.map.z, 0.0);
  float coverage = weather.r;
  if (coverage <= 0.005) return 0.0;
  float2 drift = pc.map.xy + pc.wind.xy * (pc.wind.z * h);
  float3 sp = (wp - float3(drift.x, 0.0, drift.y)) * kBaseScale;
  float4 nse = base_noise.SampleLevel(base_sampler, sp, 0.0);
  float worley_fbm = nse.g * 0.625 + nse.b * 0.25 + nse.a * 0.125;
  float base = Remap(nse.r, worley_fbm - 1.0, 1.0, 0.0, 1.0);
  base *= HeightProfile(h, weather.b, pc.fog.w * weather.g);
  return Remap(base, 1.0 - coverage, 1.0, 0.0, 1.0) * coverage;
}

float HG(float c, float g) {
  float g2 = g * g;
  return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// Closed-form optical depth of sigma0 * exp(-(y - ground)/H) along a segment.
float HeightFogOd(float y0, float dy_ds, float dist, float sigma0, float H, float ground) {
  H = max(H, 0.1);
  float e0 = exp(-max(y0 - ground, 0.0) / H);
  if (abs(dy_ds) < 1e-4) return sigma0 * e0 * dist;
  float y1 = y0 + dy_ds * dist;
  float e1 = exp(-max(y1 - ground, 0.0) / H);
  return sigma0 * (H / dy_ds) * (e0 - e1);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size.x || id.y >= pc.size.y) return;
  int2 px = int2(id.xy);
  float3 scene = color_in.Load(int3(px, 0)).rgb;

  float2 ndc = (float2(px) + 0.5) / float2(pc.size) * 2.0 - 1.0;
  float4 nh = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));  // reversed-z near
  float3 cam = pc.camera_pos.xyz;
  float3 view = normalize(nh.xyz / nh.w - cam);
  float fog_height = max(pc.fog.y, 0.1);

  float depth = depth_in.Load(int3(px, 0));
  float dist;
  if (depth > 0.0) {
    float4 wh = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
    dist = length(wh.xyz / wh.w - cam);
  } else {
    // Sky: integrate until the exponential layer has effectively run out
    // (upward rays leave it quickly, horizon rays wade through kilometres).
    dist = 24000.0;
  }
  // Cap the integral at the layer's practical ceiling so a mountain top far
  // above the fog never accumulates from the empty air between.
  float ceiling = pc.fog.z + fog_height * 7.0;
  if (view.y > 1e-3) {
    float t_top = (ceiling - cam.y) / view.y;
    if (t_top > 0.0) dist = min(dist, t_top);
    else if (cam.y > ceiling) dist = 0.0;
  }

  float sigma0 = pc.fog.x * pc.fog.x * 0.012;  // perceptual: density^2 onto extinction
  if (sigma0 <= 0.0 || dist <= 0.0) {
    out_image[px] = float4(scene, 1.0);
    return;
  }

  // Humidity and mist banks: the weather map's cells and two wind-advected
  // noise taps modulate the analytic depth, so the fog sits in patches that
  // drift downwind instead of one uniform veil.
  float2 drift = pc.map.xy * 0.6;
  float2 mid1 = cam.xz + view.xz * (dist * 0.25) - drift;
  float2 mid2 = cam.xz + view.xz * (dist * 0.7) - drift;
  // Churn scrolls the 3D noise through its third axis (two taps at different
  // rates so they slide against each other): the banks stop being a frozen
  // pattern that merely translates and start roiling -- vapour rising off the
  // water rather than a printed veil.
  float churn = pc.map.w * pc.camera_pos.w;
  float s1 = 0.31 - churn * 0.011;
  float s2 = 0.63 - churn * 0.019;
  float n1 = base_noise.SampleLevel(base_sampler, float3(mid1 * (1.0 / 1700.0), s1), 0.0).g;
  float n2 = base_noise.SampleLevel(base_sampler, float3(mid2 * (1.0 / 1700.0), s2), 0.0).b;
  // High churn also sharpens the contrast: boiling vapour is patchier than a
  // settled morning layer.
  float amp = 0.9 + 0.5 * pc.map.w;
  float banks = max(1.0 - amp * 0.5, 0.05) + amp * (n1 * 0.6 + n2 * 0.4);
  float2 wuv = (cam.xz + view.xz * (dist * 0.4) - pc.map.xy) / pc.map.z;
  float4 weather = weather_map.SampleLevel(weather_sampler, wuv, 0.0);
  float humidity = 0.75 + 0.5 * weather.g + 0.25 * weather.r;

  float od = HeightFogOd(cam.y, view.y, dist, sigma0, fog_height, pc.fog.z) * banks * humidity;

  // Steam: a second, shin-high layer boiling directly off the surface. The
  // main banks live at kilometre scale and read as a veil; this one uses
  // metre-scale noise with a fast upward scroll and a hard threshold, so
  // individual wisps with gaps between them crawl over the water. Gated by
  // churn: a settled morning layer has none, a warm swamp smokes.
  if (pc.map.w > 0.05) {
    float steam_h = 3.0;
    float steam_sigma = pc.fog.x * 0.030;
    float near_dist = min(dist, 220.0);
    float od_steam = HeightFogOd(cam.y, view.y, near_dist, steam_sigma, steam_h, pc.fog.z);
    float t = pc.camera_pos.w;
    float2 sp1 = cam.xz + view.xz * min(near_dist * 0.2, 14.0);
    float2 sp2 = cam.xz + view.xz * min(near_dist * 0.55, 48.0);
    float sn1 = base_noise.SampleLevel(base_sampler, float3(sp1 * (1.0 / 11.0), -t * 0.10), 0.0).g;
    float sn2 = base_noise.SampleLevel(base_sampler, float3(sp2 * (1.0 / 23.0), -t * 0.06), 0.0).b;
    float wisps = saturate(sn1 * 0.75 + sn2 * 0.55 - 0.32) * 2.2;
    od += od_steam * wisps * saturate(pc.map.w * 1.4);
  }

  float trans = exp(-max(od, 0.0));
  if (trans > 0.999) {
    out_image[px] = float4(scene, 1.0);
    return;
  }

  // In-scatter, sharing the deck's light model: forward-scattered sun, the
  // same sun-elevation ambient band that sets evening decks alight, murked by
  // menace, lifted by the flash. The sun colour runs through the sky's own
  // transmittance LUT, so the fog reddens with exactly the atmosphere the sky
  // renders instead of an invented tint.
  float3 to_sun = normalize(-pc.sun_direction.xyz);
  float cos_a = dot(view, to_sun);
  float sun_elev = saturate(to_sun.y * 2.2 + 0.08);
  float3 sky_tint = lerp(float3(1.0, 0.42, 0.26), float3(0.50, 0.62, 0.88), sun_elev);
  float3 warm_sun = lerp(pc.sun_color.rgb, float3(1, 1, 1), 0.35);
  float3 sun_lut = transmittance_lut
                       .SampleLevel(transmittance_sampler,
                                    TransmittanceUv(kGroundRadius + 500.0, to_sun.y), 0.0)
                       .rgb;
  float deck_shade = 1.0 - 0.62 * weather.r * (0.55 + 0.45 * weather.g);
  float dark = pc.wind.w * saturate(weather.g * 1.8);
  float phase = lerp(HG(cos_a, 0.55), 1.0 / (4.0 * kPi), 0.35);

  // God rays: the sun term is marched so the deck's actual formations gate
  // it per segment -- fog in a cloud gap glows, fog under a core sits in a
  // shadow shaft. One cheap deck tap per step at the sun ray's mid-shell
  // crossing carries the shape; the analytic transmittance stays exact.
  float sun_amt = 0.0;
  {
    float march_len = min(dist, 9000.0);
    float step = march_len / 8.0;
    float local_trans = 1.0;
    float shell_mid = lerp(pc.shell.x, pc.shell.y, 0.16);
    float thick = (pc.shell.y - pc.shell.x);
    float vis = 1.0;
    [unroll]
    for (int i = 0; i < 8; ++i) {
      float3 pi = cam + view * ((float(i) + 0.5) * step);
      float sig_i = sigma0 * exp(-max(pi.y - pc.fog.z, 0.0) / fog_height) * banks * humidity;
      if (to_sun.y > 0.06) {
        float3 ps = pi + to_sun * ((shell_mid - pi.y) / to_sun.y);
        vis = exp(-DeckDensity(ps) * thick * 0.02);
      }
      float seg = exp(-sig_i * step);
      sun_amt += local_trans * vis * (1.0 - seg);
      local_trans *= seg;
    }
    // The tail beyond the marched span keeps the last visibility estimate.
    sun_amt += max(local_trans - trans, 0.0) * vis;
    sun_amt /= max(1.0 - trans, 1e-4);  // normalized sun visibility 0..1
  }

  float3 sun_part =
      pc.sun_color.rgb * sun_lut * pc.sun_direction.w * phase * sun_amt;
  float3 amb_part = sky_tint * warm_sun * pc.sun_direction.w * 0.055 * (0.6 + 0.4 * deck_shade);
  float3 inscatter = (sun_part + amb_part) * (1.0 - 0.75 * dark) *
                     (1.0 + pc.sun_color.w * 2.5);

  out_image[px] = float4(scene * trans + inscatter * (1.0 - trans), 1.0);
}
