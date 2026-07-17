// Heightfield fluid surface, pixel stage. Shades two fluids off the shared grid
// draw: flowing water (depth-tinted absorption, IBL fresnel reflection, sun
// glint, velocity-advected flow foam) and lava (blackbody emissive from the
// temperature field, cooling crust with flow-advected cracks). The surface
// normal is central-differenced from the solver's height field; reflections use
// the environment IBL fallback only (no ray queries), mirroring water.ps.

#include "rhi_bindings.hlsli"

// Frame globals prefix (mesh_pipeline.h FrameGlobals) up to `time`. The middle
// matrices/jitter are unused here but keep the trailing fields at the right
// offset.
struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;    // xyz travel direction, w intensity
  float4 sun_color;        // rgb, w flat ambient
  float4 camera_position;  // xyz eye, w ibl intensity
  float4 misc;             // x,y render size, z sun radius, w frame index
  uint flags;
  float time;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame : register(b0, space0);

// Transient fluid set (set 1): state = (dw, dl, T, C); bed = world Y of the
// static bed; velocity = (water uv, lava uv) m/s.
[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] Texture2D<float4> state_tex : register(t0, space1);
[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] SamplerState state_samp : register(s0, space1);
[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] Texture2D<float> bed_tex : register(t1, space1);
[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] SamplerState bed_samp : register(s1, space1);
[[vk::combinedImageSampler]] [[vk::binding(2, 1)]] Texture2D<float4> vel_tex : register(t2, space1);
[[vk::combinedImageSampler]] [[vk::binding(2, 1)]] SamplerState vel_samp : register(s2, space1);

struct FluidParams {
  float2 origin;
  float extent;
  float texel;      // cell size l (meters)
  float resolution; // solver cells per side
  float3 pad;
};
[[vk::binding(3, 1)]] ConstantBuffer<FluidParams> params : register(b3, space1);

// Environment IBL set (set 2), same layout/space as water.ps.
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] TextureCube irradiance_cube : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState irradiance_samp : register(s0, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] TextureCube prefiltered_cube : register(t1, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] SamplerState prefiltered_samp : register(s1, space2);

struct FluidSurfacePush {
  float eps;
  float time;
  float water_absorption;
  float foam_scale;
  float4 absorb_color;
  float flow_period;
  float foam_speed_lo;
  float foam_speed_hi;
  float lava_emissive;
  uint grid;  // VS cell decode; unused here but the struct must mirror the CPU
  uint3 pad;
};
PUSH_CONSTANTS(FluidSurfacePush, push);

static const float kPi = 3.14159265359;

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 world_pos : TEXCOORD0;
  [[vk::location(1)]] float2 uv : TEXCOORD1;
  [[vk::location(2)]] float depth : TEXCOORD2;
  [[vk::location(3)]] float4 curr_clip : TEXCOORD3;
  [[vk::location(4)]] float4 prev_clip : TEXCOORD4;
  [[vk::location(5)]] nointerpolation uint fluid : TEXCOORD5;
};

struct PsOut {
  float4 color : SV_Target0;
  float2 motion : SV_Target1;
};

// --- value noise (self-contained, matches water.ps conventions) -------------

float Hash1(float2 p) {
  float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yzx + 33.33);
  return frac((q.x + q.y) * q.z);
}

float ValueNoise(float2 p) {
  float2 i = floor(p);
  float2 f = frac(p);
  float2 u = f * f * (3.0 - 2.0 * f);
  float a = Hash1(i);
  float b = Hash1(i + float2(1, 0));
  float c = Hash1(i + float2(0, 1));
  float d = Hash1(i + float2(1, 1));
  return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Vlachos two-phase flow-map: advect the noise sample coords with the fluid
// velocity and cross-fade the two half-period phases so the pattern flows
// without a visible reset seam. `coord` is in noise space (world meters *
// frequency), `flow` the advection vector in that same space per period.
float FlowNoise(float2 coord, float2 flow) {
  float t = push.time / max(push.flow_period, 1e-3);
  float ph0 = frac(t);
  float ph1 = frac(t + 0.5);
  float w = abs(1.0 - 2.0 * ph0);
  float a = ValueNoise(coord - flow * ph0);
  float b = ValueNoise(coord - flow * ph1);
  return lerp(a, b, w);
}

// Surface height of the fluid this pixel belongs to, sampled at an arbitrary uv
// (used for the central-difference normal). Water rides on the lava column.
float SurfaceHeight(float2 uv, uint fluid) {
  float4 st = state_tex.SampleLevel(state_samp, uv, 0.0);
  float bed = bed_tex.SampleLevel(bed_samp, uv, 0.0);
  float h = bed + st.w + st.y;       // bed + crust + lava depth
  if (fluid == 0u) h += st.x;        // + water depth
  return h;
}

float3 SkyReflection(float3 dir, float mip) {
  return min(prefiltered_cube.SampleLevel(prefiltered_samp, dir, mip).rgb, 6.0.xxx);
}

// Blackbody-ish emissive ramp for lava temperature (degrees C). Dark basalt with
// faint red below ~700, deep red at 800, orange at 1000, yellow-white by 1200.
float3 LavaEmissive(float T, out float heat) {
  float3 c700 = float3(0.05, 0.006, 0.0);
  float3 c800 = float3(0.80, 0.05, 0.0);
  float3 c1000 = float3(1.00, 0.35, 0.02);
  float3 c1200 = float3(1.00, 0.80, 0.35);
  float3 col = lerp(c700, c800, smoothstep(700.0, 800.0, T));
  col = lerp(col, c1000, smoothstep(800.0, 1000.0, T));
  col = lerp(col, c1200, smoothstep(1000.0, 1200.0, T));
  heat = smoothstep(700.0, 1200.0, T);
  return col;
}

PsOut main(PsIn input) {
  float2 uv = input.uv;
  float4 st = state_tex.SampleLevel(state_samp, uv, 0.0);  // dw, dl, T, C
  float4 vel = vel_tex.SampleLevel(vel_samp, uv, 0.0);     // water xy, lava zw

  // Central-difference normal from the own fluid's surface height, in world
  // meters (texel spacing). Fade the gradient at low depth so the wetting-front
  // meniscus does not sparkle under the temporal resolve.
  float du = 1.0 / max(params.resolution, 1.0);
  float hL = SurfaceHeight(uv - float2(du, 0), input.fluid);
  float hR = SurfaceHeight(uv + float2(du, 0), input.fluid);
  float hD = SurfaceHeight(uv - float2(0, du), input.fluid);
  float hU = SurfaceHeight(uv + float2(0, du), input.fluid);
  float2 grad = float2(hR - hL, hU - hD) / (2.0 * max(params.texel, 1e-4));
  float slope_fade = saturate(input.depth / (6.0 * push.eps));
  grad *= slope_fade;
  float3 n = normalize(float3(-grad.x, 1.0, -grad.y));

  float3 v = normalize(frame.camera_position.xyz - input.world_pos);
  float3 to_sun = normalize(-frame.sun_direction.xyz);
  float3 sun = frame.sun_color.rgb * frame.sun_direction.w;
  float3 ambient = irradiance_cube.SampleLevel(irradiance_samp, float3(0, 1, 0), 0.0).rgb;

  // Soft domain-edge + shoreline fade shared by both fluids (outer ~2 texels and
  // the sub-2*eps sliver of the wetting front).
  float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
  float edge_fade = saturate(edge / (2.0 * du));
  float depth_fade = smoothstep(push.eps, 2.0 * push.eps, input.depth);
  float shore = edge_fade * depth_fade;

  float3 color;
  float alpha;

  if (input.fluid == 1u) {
    // --- lava --------------------------------------------------------------
    float T = st.z;
    float heat;
    float3 emis = LavaEmissive(T, heat);
    // HDR emissive: hot lava blooms, cool toes barely glow.
    float3 emissive = emis * push.lava_emissive * (0.04 + heat);

    // Crust grows as the surface cools below the solidus (~800). Slow flow-mapped
    // cracks reveal the emissive underneath, scaled by remaining heat.
    float crust_amt = saturate((800.0 - T) / 300.0);
    float2 flow_l = vel.zw;
    float freq = 0.9;
    float2 coord = input.world_pos.xz * freq;
    float2 adv = flow_l * push.flow_period * freq * push.foam_scale;
    float cracks = pow(saturate(FlowNoise(coord, adv) * 1.3), 3.0);
    float3 basalt = float3(0.020, 0.018, 0.016);
    float ndl = max(dot(n, to_sun), 0.0);
    float3 lit = basalt * (sun * ndl + ambient);
    float3 crust_col = lit + emissive * cracks * heat;

    color = lerp(emissive, crust_col, crust_amt);
    alpha = shore;  // opaque body, only the shoreline sliver fades
  } else {
    // --- water -------------------------------------------------------------
    float fres = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);

    // IBL sky reflection fallback (no ray queries): reflect the view, keep it
    // above the surface so it never samples below the horizon.
    float3 r = reflect(-v, n);
    r.y = max(r.y, 0.03);
    float3 reflection = SkyReflection(normalize(r), 1.5);

    // Beer absorption over the eye path length -> opacity. Deep water tints
    // toward the blue-green absorption colour, shallow water stays clear.
    float ndv = max(dot(n, v), 0.0);
    float dw_eye = input.depth / max(0.25, ndv);
    float trans = saturate(1.0 - exp(-push.water_absorption * dw_eye));
    float3 tint = lerp(1.0.xxx, push.absorb_color.rgb, trans);
    // Body = in-scattered light, not pure absorption: the sky irradiance plus a
    // sun-driven single-scatter term, so open water reads as lit water instead
    // of a black pit when viewed head-on (fresnel is only ~2% there).
    float3 body = tint * (ambient * 1.6 + sun * 0.14 * saturate(to_sun.y));
    color = lerp(body, reflection, fres);
    alpha = lerp(0.25, 0.92, trans);

    // Sun glint: normalized Blinn-Phong on the surface normal.
    float3 h = normalize(to_sun + v);
    float ndh = max(dot(n, h), 0.0);
    color += sun * pow(ndh, 220.0) * (fres * 2.0);

    // Flow foam: onset by speed plus churn where the surface slope is steep,
    // modulated by two velocity-advected noise phases so the dam-break front
    // reads as churning white water.
    float2 flow_w = vel.xy;
    float speed = length(flow_w);
    float freq = 0.6;
    float2 coord = input.world_pos.xz * freq;
    float2 adv = flow_w * push.flow_period * freq * push.foam_scale;
    // Two octaves so the breakup has structure at both ~2 m and ~0.7 m; the
    // multiplicative gate keeps foam streaky instead of blanketing the whole
    // moving sheet white.
    float churn = FlowNoise(coord, adv) * 0.65 +
                  FlowNoise(coord * 2.7 + 17.0, adv * 2.7) * 0.35;
    float steep = saturate(length(grad) * 2.0);
    float foam = saturate(
        smoothstep(push.foam_speed_lo, push.foam_speed_hi, speed) + steep * 0.4);
    foam *= 0.15 + 0.85 * smoothstep(0.30, 0.80, churn);
    if (foam > 0.001) {
      float3 white = (sun * max(dot(n, to_sun), 0.0) * 0.25 + ambient);
      color = lerp(color, white, foam);
      alpha = lerp(alpha, 0.95, foam);
    }
    alpha *= shore;
  }

  PsOut o;
  o.color = float4(color, alpha);
  float2 curr = input.curr_clip.xy / input.curr_clip.w;
  float2 prev = input.prev_clip.xy / input.prev_clip.w;
  o.motion = (prev - curr) * 0.5;
  return o;
}
