#include "rhi_bindings.hlsli"
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D scene : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState scene_sampler : register(s0, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] Texture2D bloom : register(t1, space0);
[[vk::combinedImageSampler]] [[vk::binding(1, 0)]] SamplerState bloom_sampler : register(s1, space0);
[[vk::binding(2, 0)]] StructuredBuffer<float> exposure_buffer : register(t2, space0);  // [0] resolved exposure
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] Texture2D color_lut : register(t3, space0);  // 1024x32 strip lut
[[vk::combinedImageSampler]] [[vk::binding(3, 0)]] SamplerState color_lut_sampler : register(s3, space0);
// Tight 1/4-res highlight snapshot off the bloom down chain (pre up-chain
// widening); the lens-flare ghosts sample this, not the wide final bloom.
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2D flare_src : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState flare_src_sampler : register(s4, space0);

struct PushData {
  uint tonemap;  // 0 aces, 1 reinhard, 2 none, 3 agx
  float bloom_intensity;
  uint bloom_enabled;
  uint lut_enabled;
  // Output encode: 0 sRGB (SDR), 1 HDR10 PQ, 2 scRGB linear fp16. The HDR
  // modes are SDR-referred for now: the tonemapped [0,1] signal maps to
  // paper_white nits, so grading/LUTs stay valid; highlight-through HDR
  // grading is a later stage.
  uint output_transfer;
  float paper_white;  // nits of tonemapped 1.0 in the HDR modes
  // Lens package: ghosts/halo feed off the bloom chain, aberration shifts the
  // scene fetch radially, vignette+grain apply post-tonemap.
  float flare_intensity;      // 0 disables the ghost/halo sampling
  float aberration;           // px of radial r/b shift at the corners
  float vignette;             // 0..1 corner darkening
  float grain;                // 0..~0.06 film grain amplitude
  float grain_seed;           // per-frame
  float pad_lens[3];
};
PUSH_CONSTANTS(PushData, push);

// Strip color lut: 32 blue slices laid out horizontally (1024x32). Hardware
// bilinear covers red/green within a slice; blue is a manual lerp across slices.
float3 ApplyColorLut(float3 c) {
  const float size = 32.0;
  c = saturate(c);
  float blue = c.b * (size - 1.0);
  float slice = floor(blue);
  float frac_b = blue - slice;
  float u = (c.r * (size - 1.0) + 0.5) / (size * size);
  float v = (c.g * (size - 1.0) + 0.5) / size;
  float3 a = color_lut.SampleLevel(color_lut_sampler, float2(u + slice / size, v), 0.0).rgb;
  float3 b = color_lut.SampleLevel(color_lut_sampler,
                                   float2(u + min(slice + 1.0, size - 1.0) / size, v), 0.0).rgb;
  return lerp(a, b, frac_b);
}

// Narkowicz ACES fit. Cheap, no LUT, good enough until a proper grading
// stage with white balance lands.
float3 TonemapAces(float3 x) {
  return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// AgX (Sobotka), Filament-style polynomial fit. Wide-shoulder log encode with
// an inset gamut: bright saturated light desaturates smoothly toward white
// instead of clipping per channel (the ACES fit turns hot foliage/sky into
// flat white patches and skews hues near clip).
float3 AgxContrast(float3 x) {
  float3 x2 = x * x;
  float3 x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
         0.1191 * x - 0.00232;
}
float3 TonemapAgx(float3 c) {
  const float3x3 agx_mat = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
  const float3x3 agx_mat_inv =
      float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
               -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
               -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
  const float min_ev = -12.47393;
  const float max_ev = 4.026069;
  c = mul(c, agx_mat);
  c = clamp(log2(max(c, 1e-10)), min_ev, max_ev);
  c = (c - min_ev) / (max_ev - min_ev);
  c = AgxContrast(c);
  c = mul(c, agx_mat_inv);
  // The fit outputs a 2.2-encoded value; back to linear for the output encode.
  return pow(saturate(c), 2.2);
}

// The swapchain is UNORM, the engine owns the transfer function.
float3 SrgbEncode(float3 c) {
  return lerp(c * 12.92, 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

// Rec.709 -> Rec.2020 primaries (HDR10 containers are Rec.2020-coded).
float3 Rec709ToRec2020(float3 c) {
  return float3(dot(float3(0.6274, 0.3293, 0.0433), c),
                dot(float3(0.0691, 0.9195, 0.0114), c),
                dot(float3(0.0164, 0.0880, 0.8956), c));
}

// SMPTE ST 2084 (PQ) OETF; input in nits / 10000.
float3 PqEncode(float3 n) {
  const float m1 = 0.1593017578125, m2 = 78.84375;
  const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
  float3 p = pow(max(n, 0.0), m1);
  return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

// Cheap hash for the film grain.
float GrainHash(float2 p) {
  return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float4 main(float4 sv_position : SV_Position,
            [[vk::location(0)]] float2 uv : TEXCOORD0) : SV_Target0 {
  float3 hdr;
  if (push.aberration > 0.0) {
    // Radial chromatic aberration: r and b converge at different radii, most
    // visible toward the corners like a real fast lens.
    float2 dims;
    scene.GetDimensions(dims.x, dims.y);
    float2 from_center = uv - 0.5;
    float2 shift = from_center * dot(from_center, from_center) * 4.0 *
                   (push.aberration / dims.x);
    hdr = float3(scene.Sample(scene_sampler, uv + shift).r,
                 scene.Sample(scene_sampler, uv).g,
                 scene.Sample(scene_sampler, uv - shift).b);
  } else {
    hdr = scene.Sample(scene_sampler, uv).rgb;
  }
  if (push.bloom_enabled != 0u) {
    hdr = lerp(hdr, bloom.Sample(bloom_sampler, uv).rgb, push.bloom_intensity);
  }
  if (push.flare_intensity > 0.0) {
    // Ghosts: bright sources mirrored through the lens center at a few
    // characteristic scales, tinted cooler with distance; plus a halo ring.
    // Sampled from the tight flare_src snapshot (the wide final bloom smears
    // every ghost into a screen-filling halo), and highpassed so only the
    // excess above display white flares: the source is threshold-free, so raw
    // it mirrors the whole scene back as a milky wash. The threshold is in
    // pre-exposure space, scaled by the resolved exposure so it tracks
    // auto-exposure.
    float flare_thresh = 1.0 / max(exposure_buffer[0], 1e-4);
    float2 flipped = 1.0 - uv;
    float3 ghosts = 0.0.xxx;
    const float scales[4] = {-0.35, -0.65, 0.4, 0.8};
    const float weights[4] = {0.5, 0.25, 0.35, 0.15};
    [unroll]
    for (int g = 0; g < 4; ++g) {
      float2 guv = 0.5 + (flipped - 0.5) * scales[g];
      float edge_fade = saturate(1.0 - 2.2 * length(guv - 0.5));
      float3 hot = max(flare_src.Sample(flare_src_sampler, guv).rgb - flare_thresh, 0.0);
      ghosts += hot * weights[g] * edge_fade;
    }
    float2 from_center = uv - 0.5;
    float halo_r = length(from_center);
    float2 halo_uv = 0.5 + normalize(from_center + 1e-5) * 0.45;
    float halo_w = exp(-abs(halo_r - 0.42) * 18.0);
    ghosts +=
        max(flare_src.Sample(flare_src_sampler, halo_uv).rgb - flare_thresh, 0.0) * halo_w * 0.4;
    // Soft-clip so a blown-out source (the 220x sun disk) cannot dump unbounded
    // energy back onto the frame; cool tint keeps the ghosts reading as glass.
    ghosts = ghosts / (1.0 + ghosts);
    hdr += ghosts * float3(0.7, 0.85, 1.0) * (push.flare_intensity * 8.0);
  }
  hdr *= exposure_buffer[0];

  float3 ldr;
  if (push.tonemap == 0u) {
    ldr = TonemapAces(hdr);
  } else if (push.tonemap == 1u) {
    ldr = hdr / (1.0 + hdr);
  } else if (push.tonemap == 3u) {
    ldr = TonemapAgx(hdr);
  } else {
    ldr = saturate(hdr);
  }
  if (push.lut_enabled != 0u) ldr = ApplyColorLut(ldr);
  if (push.vignette > 0.0) {
    float2 vc = uv - 0.5;
    float v = 1.0 - push.vignette * smoothstep(0.25, 0.85, dot(vc, vc) * 2.0);
    ldr *= v;
  }
  if (push.grain > 0.0) {
    // Luma-weighted grain: strongest in the midtones, fades in deep shadow
    // and near white like negative stock.
    float g = GrainHash(uv * 1237.0 + push.grain_seed) - 0.5;
    float luma = dot(ldr, float3(0.2126, 0.7152, 0.0722));
    ldr += g * push.grain * (1.0 - abs(luma * 2.0 - 1.0)) * 0.5 + g * push.grain * 0.5;
  }
  if (push.output_transfer == 1u) {  // HDR10 PQ
    float3 nits = Rec709ToRec2020(ldr) * push.paper_white;
    return float4(PqEncode(nits / 10000.0), 1.0);
  }
  if (push.output_transfer == 2u) {  // scRGB linear, 1.0 = 80 nits
    return float4(ldr * (push.paper_white / 80.0), 1.0);
  }
  return float4(SrgbEncode(ldr), 1.0);
}
