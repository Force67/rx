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

struct HazePush {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;     // xyz eye (m), w time (s)
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w flash 0..1
  float4 wind;           // xy blow direction (unit XZ), z speed m/s, w darkness
  float4 fog;            // x density 0..1, y falloff height (m), z ground (m), w anvil
  float4 map;            // xy map offset (m), z map extent (m), w churn 0..1
  uint2 size;
  float2 _pad;
};
PUSH_CONSTANTS(HazePush, pc);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> color_in : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float> depth_in : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture3D<float4> base_noise : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState base_sampler : register(s3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2D<float4> weather_map : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState weather_sampler : register(s4, space0);

static const float kPi = 3.14159265359;

float HG(float c, float g) {
  float g2 = g * g;
  return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// Closed-form optical depth of sigma0 * exp(-(y - ground)/H) along a segment.
float HeightFogOd(float y0, float dy_ds, float dist, float sigma0, float H, float ground) {
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
  float ceiling = pc.fog.z + pc.fog.y * 7.0;
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
  float2 drift = pc.wind.xy * (pc.wind.z * pc.camera_pos.w * 0.6);
  float2 mid1 = cam.xz + view.xz * (dist * 0.25) + drift;
  float2 mid2 = cam.xz + view.xz * (dist * 0.7) + drift;
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
  float2 wuv = (cam.xz + view.xz * (dist * 0.4) + pc.map.xy) / pc.map.z;
  float4 weather = weather_map.SampleLevel(weather_sampler, wuv, 0.0);
  float humidity = 0.75 + 0.5 * weather.g + 0.25 * weather.r;

  float od = HeightFogOd(cam.y, view.y, dist, sigma0, pc.fog.y, pc.fog.z) * banks * humidity;

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

  // In-scatter, sharing the deck's light model: forward-scattered sun (the
  // hazy glare around a low sun comes free from the phase), the same
  // sun-elevation ambient band that sets evening decks alight, dimmed where
  // the deck overhead is thick, murked by menace, lifted by the flash.
  float3 to_sun = normalize(-pc.sun_direction.xyz);
  float cos_a = dot(view, to_sun);
  float sun_elev = saturate(to_sun.y * 2.2 + 0.08);
  float3 sky_tint = lerp(float3(1.0, 0.42, 0.26), float3(0.50, 0.62, 0.88), sun_elev);
  float3 warm_sun = lerp(pc.sun_color.rgb, float3(1, 1, 1), 0.35);
  float deck_shade = 1.0 - 0.62 * weather.r * (0.55 + 0.45 * weather.g);
  float dark = pc.wind.w * saturate(weather.g * 1.8);
  float phase = lerp(HG(cos_a, 0.55), 1.0 / (4.0 * kPi), 0.35);
  float3 sun_part = pc.sun_color.rgb * pc.sun_direction.w * phase * deck_shade;
  float3 amb_part = sky_tint * warm_sun * pc.sun_direction.w * 0.055 * (0.6 + 0.4 * deck_shade);
  float3 inscatter = (sun_part + amb_part) * (1.0 - 0.75 * dark) *
                     (1.0 + pc.sun_color.w * 2.5);

  out_image[px] = float4(scene * trans + inscatter * (1.0 - trans), 1.0);
}
