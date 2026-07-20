#include "rhi_bindings.hlsli"
// Cloudscape tornado funnel: a small marched vortex volume between the ground
// and the cloud base, driven by the weather layer's lifecycle (touchdown,
// wander, rope-out). The wall is a hollow cone of noise: narrow at the
// ground, flaring toward the base, its axis snaking with height and time and
// the noise streaking AROUND the axis fast enough to read as rotation. A
// dust skirt churns at the contact point. The march is bounded by a
// ray/cylinder test around the axis, so pixels that never touch the vortex
// pay two dot products and exit.

struct FunnelPush {
  column_major float4x4 inv_view_proj;
  float4 camera_pos;  // xyz eye (m), w time (s)
  float4 sun_color;   // rgb sun colour, w flash 0..1
  float4 funnel;      // xy axis ground pos, z wall radius (m), w strength 0..1
  float4 bounds;      // x ground y (m), y cloud base y (m), z sun intensity, w darkness
  float2 jitter;
  float2 _pad;
};
[[vk::binding(4, 0)]] ConstantBuffer<FunnelPush> pc : register(b4, space0);

[[vk::binding(0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> out_image : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float4> color_in : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float> depth_in : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture3D<float4> base_noise : register(t3, space0);
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState base_sampler : register(s3, space0);

static const float kTwoPi = 6.28318530718;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  out_image.GetDimensions(width, height);
  uint2 size = uint2(width, height);
  if (id.x >= size.x || id.y >= size.y) return;
  int2 px = int2(id.xy);
  float3 scene = color_in.Load(int3(px, 0)).rgb;

  float strength = pc.funnel.w;
  float2 ndc = (float2(px) + 0.5) / float2(size) * 2.0 - 1.0 - pc.jitter;
  float4 nh = mul(pc.inv_view_proj, float4(ndc, 1.0, 1.0));  // reversed-z near
  float3 cam = pc.camera_pos.xyz;
  float3 view = normalize(nh.xyz / nh.w - cam);

  // Bounding cylinder around the (vertical) axis, generous enough for the
  // flare, the axis wobble and the dust skirt.
  float ground = pc.bounds.x;
  float base_y = pc.bounds.y;
  float r_bound = pc.funnel.z * 3.4 + 40.0;
  float2 oc = cam.xz - pc.funnel.xy;
  float a = dot(view.xz, view.xz);
  float b = dot(oc, view.xz);
  float c = dot(oc, oc) - r_bound * r_bound;
  float t0, t1;
  if (a < 1e-6) {
    if (c > 0.0) {
      out_image[px] = float4(scene, 1.0);
      return;
    }
    t0 = 0.0;
    t1 = 1e6;  // looking straight up/down inside the cylinder
  } else {
    float disc = b * b - a * c;
    if (disc <= 0.0) {
      out_image[px] = float4(scene, 1.0);
      return;
    }
    float sq = sqrt(disc);
    t0 = max((-b - sq) / a, 0.0);
    t1 = (-b + sq) / a;
  }

  // Clamp to the vertical slab and the scene.
  float depth = depth_in.Load(int3(px, 0));
  float scene_dist = 1e12;
  if (depth > 0.0) {
    float4 wh = mul(pc.inv_view_proj, float4(ndc, depth, 1.0));
    scene_dist = length(wh.xyz / wh.w - cam);
  }
  if (abs(view.y) > 1e-4) {
    float ta = (ground - cam.y) / view.y;
    float tb = (base_y + 120.0 - cam.y) / view.y;
    t0 = max(t0, min(ta, tb));
    t1 = min(t1, max(ta, tb));
  } else if (cam.y < ground || cam.y > base_y + 120.0) {
    out_image[px] = float4(scene, 1.0);
    return;
  }
  t1 = min(t1, scene_dist);
  if (t1 <= t0 || strength <= 0.01) {
    out_image[px] = float4(scene, 1.0);
    return;
  }

  float t = pc.camera_pos.w;
  float trans = 1.0;
  float3 scatter = 0.0.xxx;
  const int kSteps = 26;
  float dt = (t1 - t0) / float(kSteps);
  // The funnel is unlit geometry-scale volume: ambient grey-brown, darker
  // toward the ground where the dust loads it, flash-lit like the deck.
  float3 wall_col = lerp(float3(0.16, 0.15, 0.14), float3(0.38, 0.37, 0.36),
                         saturate(1.0 - pc.bounds.w)) *
                    pc.bounds.z * 0.14;
  float3 dust_col = float3(0.30, 0.26, 0.20) * pc.bounds.z * 0.12;
  float flash_gain = 1.0 + pc.sun_color.w * 5.0;

  [loop]
  for (int i = 0; i < kSteps; ++i) {
    float s = t0 + (float(i) + 0.5) * dt;
    float3 p = cam + view * s;
    float h = saturate((p.y - ground) / max(base_y - ground, 1.0));

    // Sinuous axis: the funnel leans and snakes more the higher you look.
    float2 axis = pc.funnel.xy +
                  float2(sin(h * 5.1 + t * 0.7), cos(h * 3.9 + t * 0.55)) * (14.0 * h + 2.0);
    float2 d2 = p.xz - axis;
    float r = length(d2);

    // Wall radius flares from a tight ground contact to the base; the wall
    // itself is a soft hollow shell, filled-in near the top.
    float rw = pc.funnel.z * (0.20 + 1.9 * pow(h, 1.35));
    float wall_width = max(rw * 0.42, 4.0);
    float dust_width = max(rw * 1.3, 8.0);
    bool near_wall = abs(r - rw) < wall_width * 3.0 ||
                     (h > 0.28 && r < max(rw, 4.0) * 2.5);
    bool near_dust = h < 0.15 && abs(r - rw * 2.4) < dust_width * 3.0;
    if (!near_wall && !near_dust) continue;
    float wall_q = (r - rw) / wall_width;
    float core_q = r / max(rw, 4.0);
    float wall = exp(-wall_q * wall_q);
    wall = max(wall, exp(-core_q * core_q) * saturate(h * 1.8 - 0.5));

    // Rotation: streak the noise around the axis. The angle coordinate maps
    // onto the tileable noise with an integer frequency, so there is no seam.
    float ang = r > 1e-4 ? atan2(d2.y, d2.x) / kTwoPi : 0.0;
    float3 nc = float3(ang * 3.0 + t * 0.55 + h * 2.6, h * 2.3 - t * 0.42, 0.71);
    float streak = base_noise.SampleLevel(base_sampler, nc, 0.0).g;

    // The condensation funnel thins optically toward the base flare; weight
    // the lower half harder or the ground contact dissolves into the haze.
    float dens = strength * wall * (0.45 + 0.75 * streak) * (1.35 - 0.55 * h);
    // Dust skirt: a wider, denser churn hugging the contact point.
    float dust_q = (r - rw * 2.4) / dust_width;
    float dust = exp(-dust_q * dust_q) *
                 saturate(1.0 - h * 7.0) * strength * (0.6 + 0.8 * streak);

    float sigma = (dens * 0.050 + dust * 0.085);
    if (sigma > 1e-5) {
      float step_trans = exp(-sigma * dt);
      float w = trans * (1.0 - step_trans);
      float3 col = (wall_col * dens + dust_col * dust) / max(dens + dust, 1e-4);
      scatter += w * col * (0.45 + 0.55 * h) * flash_gain;
      trans *= step_trans;
      if (trans < 0.02) break;
    }
  }

  out_image[px] = float4(scene * trans + scatter, 1.0);
}
