// Water surface: fbm wave normals, raytraced reflections shaded through the
// bindless scene tables (sky on miss), screen space refraction with beer
// absorption against the opaque snapshot, fresnel weighting and a ggx sun
// glint. Material base color acts as the absorption tint.

#include "rhi_bindings.hlsli"

struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
  float4 sun_direction;  // xyz travel direction, w intensity
  float4 sun_color;      // rgb, w flat ambient
  float4 camera_position;  // xyz eye, w ibl intensity
  float4 misc;             // x,y render size, z sun radius, w frame index
  uint flags;
  float time;
  // Full mirror of mesh_pipeline.h FrameGlobals from here on, so the trailing
  // water_* params (appended last) land at the right offset. Only the water
  // block below is read by this shader; the middle fields are pad-through.
  uint debug_view;
  float reflection_cutoff;
  uint ao_ray_count;
  uint light_count;
  float2 pad_wind;
  float4 wind;
  float4 cluster_params;
  float4 interior_ambient;
  float4 interior_fog_color0;
  float4 interior_fog_color1;
  float4 interior_fog_params;
  float4 shore_field;
  float4 water_absorption;  // rgb Beer coeff (1/m), w overall scale
  float4 water_material;    // x transmission, y refl foam gain, z crest-sss intensity, w crest-sss exponent
  float4 water_caustics;    // x intensity, y rest height, z depth-fade, w unused
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame : register(b0, space0);
[[vk::binding(1, 0)]] RaytracingAccelerationStructure tlas : register(t1, space0);

struct MaterialParams {
  float4 base_color_factor;
  float3 emissive_factor;
  float metallic_factor;
  float roughness_factor;
  float alpha_cutoff;
  uint flags;
  float pad;
};
[[vk::binding(0, 1)]] ConstantBuffer<MaterialParams> material : register(b0, space1);

[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] TextureCube irradiance_cube : register(t0, space2);
[[vk::combinedImageSampler]] [[vk::binding(0, 2)]] SamplerState irradiance_sampler : register(s0, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] TextureCube prefiltered_cube : register(t1, space2);
[[vk::combinedImageSampler]] [[vk::binding(1, 2)]] SamplerState prefiltered_sampler : register(s1, space2);
[[vk::combinedImageSampler]] [[vk::binding(2, 2)]] Texture2D brdf_lut : register(t2, space2);
[[vk::combinedImageSampler]] [[vk::binding(2, 2)]] SamplerState brdf_lut_sampler : register(s2, space2);
[[vk::combinedImageSampler]] [[vk::binding(3, 2)]] Texture2D ao_map : register(t3, space2);
[[vk::combinedImageSampler]] [[vk::binding(3, 2)]] SamplerState ao_sampler : register(s3, space2);
[[vk::combinedImageSampler]] [[vk::binding(4, 2)]] Texture2DArray ddgi_irradiance : register(t4, space2);
[[vk::combinedImageSampler]] [[vk::binding(4, 2)]] SamplerState ddgi_irradiance_sampler : register(s4, space2);
[[vk::combinedImageSampler]] [[vk::binding(5, 2)]] Texture2DArray ddgi_distance : register(t5, space2);
[[vk::combinedImageSampler]] [[vk::binding(5, 2)]] SamplerState ddgi_distance_sampler : register(s5, space2);

struct DdgiVolume {
  float4 origin;
  uint4 counts;
  float4 params;
};
[[vk::binding(6, 2)]] ConstantBuffer<DdgiVolume> ddgi : register(b6, space2);

#define RX_GEOMETRY_SPACE space3
#include "rt_geometry.hlsli"
struct MaterialRecord {
  float4 base_color_factor;
  float3 emissive;
  uint base_color_texture;
  uint flags;
  float alpha_cutoff;
  float roughness;
  float metallic;
  uint metallic_roughness_texture;
  uint pad0;
  uint pad1;
  uint pad2;
};
[[vk::binding(0, 3)]] StructuredBuffer<MeshRecord> mesh_records : register(t0, space3);
[[vk::binding(1, 3)]] StructuredBuffer<GeometryRecord> geometry_records : register(t1, space3);
[[vk::binding(2, 3)]] StructuredBuffer<MaterialRecord> material_records : register(t2, space3);
[[vk::binding(3, 3)]] Texture2D bindless_textures[RX_BINDLESS_TEXTURE_COUNT] : register(t3, space3);
[[vk::binding(4, 3)]] SamplerState bindless_sampler : register(s4, space3);

[[vk::combinedImageSampler]] [[vk::binding(0, 4)]] Texture2D opaque_color : register(t0, space4);
[[vk::combinedImageSampler]] [[vk::binding(0, 4)]] SamplerState opaque_color_sampler : register(s0, space4);
[[vk::combinedImageSampler]] [[vk::binding(1, 4)]] Texture2D opaque_depth : register(t1, space4);
[[vk::combinedImageSampler]] [[vk::binding(1, 4)]] SamplerState opaque_depth_sampler : register(s1, space4);

static const uint kFrameDdgi = 4u;
static const uint kFrameWaterRt = 8u;
static const float kPi = 3.14159265359;

struct PsIn {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float4 curr_clip : TEXCOORD1;
  [[vk::location(2)]] float4 prev_clip : TEXCOORD2;
  [[vk::location(3)]] float3 world_pos : TEXCOORD3;
  [[vk::location(4)]] float4 tangent : TANGENT;
  [[vk::location(5)]] float2 uv : TEXCOORD0;
  [[vk::location(6)]] float4 color : COLOR0;
};

struct PsOut {
  float4 color : SV_Target0;
  float2 motion : SV_Target1;
};

// --- waves -----------------------------------------------------------------

float2 Hash2(float2 p) {
  float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
  q += dot(q, q.yzx + 33.33);
  return frac((q.xx + q.yz) * q.zy) * 2.0 - 1.0;
}

float GradNoise(float2 p) {
  float2 i = floor(p);
  float2 f = frac(p);
  float2 u = f * f * (3.0 - 2.0 * f);
  float a = dot(Hash2(i), f);
  float b = dot(Hash2(i + float2(1, 0)), f - float2(1, 0));
  float c = dot(Hash2(i + float2(0, 1)), f - float2(0, 1));
  float d = dot(Hash2(i + float2(1, 1)), f - float2(1, 1));
  return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float WaveHeight(float2 p, float t) {
  // Few low octaves: high frequency shimmer would just be eaten by the
  // temporal passes, slow broad waves survive them.
  float h = 0.0;
  float amp = 1.0;
  float freq = 1.0;
  float2 drift = float2(0.35, 0.21);
  [unroll]
  for (int i = 0; i < 3; ++i) {
    h += amp * GradNoise(p * freq + drift * t * freq);
    amp *= 0.5;
    freq *= 2.3;
    drift = float2(-drift.y, drift.x) * 1.2;
  }
  return h;
}

float3 WaveNormal(float2 p, float t, float strength) {
  const float eps = 0.08;
  float h0 = WaveHeight(p, t);
  float hx = WaveHeight(p + float2(eps, 0), t);
  float hz = WaveHeight(p + float2(0, eps), t);
  return normalize(float3(-(hx - h0) / eps * strength, 1.0, -(hz - h0) / eps * strength));
}

// --- ddgi sampling (matches mesh.ps) ----------------------------------------

float2 OctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}

float2 ProbeAtlasUv(uint3 probe, float3 dir, float texels, float2 atlas_size) {
  float2 oct = OctEncode(dir) * 0.5 + 0.5;
  float2 base = float2(probe.x + probe.z * ddgi.counts.x, probe.y) * (texels + 2.0) + 1.0;
  return (base + 0.5 + oct * (texels - 1.0)) / atlas_size;
}

float3 SampleDdgiNearest(float3 world_pos, float3 n) {
  if ((frame.flags & kFrameDdgi) == 0u) return 0.0.xxx;
  float3 local = (world_pos - ddgi.origin.xyz) / ddgi.origin.w;
  if (any(local < 0.0) || any(local > float3(ddgi.counts.xyz - 1))) return 0.0.xxx;
  uint3 probe = (uint3)round(local);
  float texels = (float)ddgi.counts.w;
  float2 atlas = float2((ddgi.counts.w + 2) * ddgi.counts.x * ddgi.counts.z,
                        (ddgi.counts.w + 2) * ddgi.counts.y);
  float3 irr = ddgi_irradiance
      .SampleLevel(ddgi_irradiance_sampler,
                   float3(ProbeAtlasUv(probe, n, texels, atlas), 0.0), 0.0).rgb;
  return irr * irr * ddgi.params.w;
}

// --- reflection -------------------------------------------------------------

// The sky cube carries the raw sun disk for bloom; reflections must not,
// the analytic glint term owns sun reflection. Blurred mip + clamp.
float3 SkyReflection(float3 dir, float mip) {
  return min(prefiltered_cube.SampleLevel(prefiltered_sampler, dir, mip).rgb, 6.0.xxx);
}

float3 TraceReflection(float3 origin, float3 dir) {
  if ((frame.flags & kFrameWaterRt) == 0u) {
    return SkyReflection(dir, 2.0);
  }
  RayDesc ray;
  ray.Origin = origin + float3(0.0, 0.05, 0.0);
  ray.TMin = 0.01;
  ray.Direction = dir;
  ray.TMax = 500.0;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
  rq.TraceRayInline(tlas, RAY_FLAG_NONE, RX_RAY_MASK_REALTIME, ray);
  rq.Proceed();
  if (rq.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    return SkyReflection(dir, 1.0);
  }

  float3 hit_pos = origin + dir * rq.CommittedRayT();
  MeshRecord mesh = mesh_records[NonUniformResourceIndex(rq.CommittedInstanceID())];
  GeometryRecord geometry = geometry_records[mesh.geometry_offset + rq.CommittedGeometryIndex()];
  uint3 tri =
      RxLoadTriangle(mesh, geometry.index_offset + rq.CommittedPrimitiveIndex() * 3);
  float2 bary = rq.CommittedTriangleBarycentrics();
  float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
  float3 n_local = 0.0.xxx;
  float2 uv = 0.0.xx;
  [unroll]
  for (uint corner = 0; corner < 3; ++corner) {
    n_local += RxLoadNormal(mesh, tri[corner]) * w[corner];
    uv += RxLoadUv(mesh, tri[corner]) * w[corner];
  }
  float3x4 to_world = rq.CommittedObjectToWorld3x4();
  float3 n = normalize(mul((float3x3)to_world, n_local));
  if (dot(n, dir) > 0.0) n = -n;

  MaterialRecord hit_material =
      material_records[NonUniformResourceIndex(geometry.material_index)];
  float3 albedo = hit_material.base_color_factor.rgb;
  if (hit_material.base_color_texture != 0xffffffffu) {
    albedo *= bindless_textures[NonUniformResourceIndex(hit_material.base_color_texture)]
                  .SampleLevel(bindless_sampler, uv, 3.0).rgb;
  }
  float3 to_sun = normalize(-frame.sun_direction.xyz);
  float3 sun = frame.sun_color.rgb * frame.sun_direction.w;
  float ndl = max(dot(n, to_sun), 0.0);
  return albedo / kPi * sun * ndl + albedo * SampleDdgiNearest(hit_pos, n) +
         hit_material.emissive;
}

// --- surface ----------------------------------------------------------------

#include "water_waves.hlsli"
#include "water_field.hlsli"

static const uint kFrameWaterField = 8192u;  // 1 << 13, mirrors mesh_pipeline.h

PsOut main(PsIn input) {
  float3 v = normalize(frame.camera_position.xyz - input.world_pos);
  float view_dist = length(frame.camera_position.xyz - input.world_pos);

  // Broad shape from the same Gerstner field that displaced the vertices
  // (evaluated at the displaced footprint: the error is a fraction of the
  // chop and invisible), fine anisotropic ripple detail layered on top and
  // faded with distance so far water stays calm under taa.
  float3 gerstner_n;
  float crest;
  if ((frame.flags & 2048u) != 0u) {  // kFrameFftOcean
    OceanDisplace(input.world_pos.xz, gerstner_n, crest);
  } else {
    GerstnerWave(input.world_pos.xz, frame.time, gerstner_n, crest);
  }
  float strength = lerp(0.045, 0.008, saturate(view_dist / 250.0)) *
                   saturate(material.roughness_factor * 16.0);
  float3 detail = WaveNormal(input.world_pos.xz * float2(2.6, 1.4), frame.time * 0.7, strength);
  float3 n = normalize(float3(gerstner_n.xz + detail.xz, gerstner_n.y * detail.y).xzy);

  // Persistent field: advected foam + near-camera interactive ripples (object
  // wakes, the ripple wave equation). Sampled once by world xz; zero when off.
  float4 field = 0.0.xxxx;
  if ((frame.flags & kFrameWaterField) != 0u) {
    field = SampleWaterField(input.world_pos.xz);
    // Ripple-height gradient perturbs the shading normal, faded out with
    // distance so only the detailed near ring contributes (ring 0 ~48 m).
    float ripple_fade = saturate(1.0 - view_dist / 60.0);
    if (ripple_fade > 0.0) {
      const float e = 0.25;  // meters
      float hx = WaterFieldHeight(input.world_pos.xz + float2(e, 0.0)) -
                 WaterFieldHeight(input.world_pos.xz - float2(e, 0.0));
      float hz = WaterFieldHeight(input.world_pos.xz + float2(0.0, e)) -
                 WaterFieldHeight(input.world_pos.xz - float2(0.0, e));
      n = normalize(n + float3(-hx, 0.0, -hz) * (ripple_fade * 1.2 / (2.0 * e)));
    }
  }

  // Refraction against the opaque snapshot, distorted by the waves.
  float2 screen_uv = input.sv_position.xy / frame.misc.xy;
  float2 distortion = n.xz * (0.015 / max(view_dist * 0.08, 1.0));
  float2 refracted_uv = clamp(screen_uv + distortion, 0.001, 0.999);
  float behind_depth = opaque_depth.SampleLevel(opaque_depth_sampler, refracted_uv, 0).r;
  if (behind_depth > input.sv_position.z) {
    // The distorted sample lands on geometry in front of the water.
    refracted_uv = screen_uv;
    behind_depth = opaque_depth.SampleLevel(opaque_depth_sampler, refracted_uv, 0).r;
  }
  // Empty depth means open water to the horizon; reconstructing infinity
  // would breed NaNs that temporal passes smear everywhere.
  float water_depth = 200.0;
  if (behind_depth > 1e-6) {
    float2 ndc = refracted_uv * 2.0 - 1.0;
    float4 behind_world = mul(frame.inv_view_proj, float4(ndc, behind_depth, 1.0));
    behind_world.xyz /= behind_world.w;
    water_depth = min(length(behind_world.xyz - input.world_pos), 200.0);
  }

  // Beer-Lambert absorption over the refracted path length (water_depth): the
  // per-channel coefficients (red dies first) turn deep water blue-green while
  // shallow water stays clear. Tunable through the [water] settings.
  float3 absorption = frame.water_absorption.rgb * frame.water_absorption.w;
  float3 transmittance = exp(-absorption * water_depth);

  // Caustics on the refracted seafloor: two counter-scrolling ridge-noise
  // sheets multiplied (the classic web pattern), attenuated by depth and sun.
  float3 caustic = 0.0.xxx;
  if (behind_depth > 1e-6 && water_depth < 12.0) {
    float2 ndc_b = refracted_uv * 2.0 - 1.0;
    float4 bw = mul(frame.inv_view_proj, float4(ndc_b, behind_depth, 1.0));
    float2 floor_xz = bw.xyz.xz / bw.w;
    float t = frame.time;
    float c1 = 1.0 - abs(WaveHeight(floor_xz * 1.9 + float2(t * 0.31, t * 0.17), t * 0.6));
    float c2 = 1.0 - abs(WaveHeight(floor_xz * 2.3 - float2(t * 0.23, t * 0.29), t * 0.7));
    float web = pow(saturate(c1 * c2), 6.0);
    float sun_up = saturate(-normalize(frame.sun_direction.xyz).y);
    caustic = frame.sun_color.rgb * frame.sun_direction.w * web *
              exp(-water_depth * 0.5) * sun_up * 0.6;
  }
  // Single-scatter body colour for water too deep to see through. The 0.35
  // keeps it a deep sea-blue: full upward irradiance reads as milky turquoise.
  float3 scatter = material.base_color_factor.rgb * 0.35 *
                   irradiance_cube.SampleLevel(irradiance_sampler, float3(0, 1, 0), 0).rgb;
  float3 refracted = opaque_color.SampleLevel(opaque_color_sampler, refracted_uv, 0).rgb +
                     caustic;
  // Transmission strength scales how much of the refracted floor survives vs the
  // scattered water body colour (foam reduces it further below).
  float3 below = lerp(scatter, refracted, saturate(transmittance * frame.water_material.x));

  // Foam density + ripple energy roughen the reflection: foamy/choppy water
  // scatters its mirror into a broad, dimmer response instead of a sharp sky.
  float foam_density = 1.0 - exp(-1.1 * field.b);
  float ripple_energy = saturate(abs(field.g) * 2.0 + length(detail.xz) * 3.0);
  float refl_rough = saturate((foam_density + ripple_energy * 0.5) * frame.water_material.y);

  // Reflection, kept above the surface so the trace never self-hits.
  float3 r = reflect(-v, n);
  r.y = max(r.y, 0.03);
  float3 reflection = TraceReflection(input.world_pos, normalize(r));
  if (refl_rough > 0.001) {
    // Widen toward a blurred sky mip and dim; the sharp RT/mirror term fades as
    // the surface roughens so rough water reads matte rather than glassy.
    float3 broad = SkyReflection(normalize(r), lerp(2.0, 5.0, refl_rough));
    reflection = lerp(reflection, broad, refl_rough * 0.7) * lerp(1.0, 0.6, refl_rough);
  }

  float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
  float3 color = lerp(below, reflection, fresnel);

  // Wave subsurface scattering: sun shining through a thin, lifted crest from
  // behind scatters forward to the camera. Thickness is proxied from the crest
  // factor (pinched tops are thin) and the height above rest; the transmission
  // lobe attenuates by exp(-thickness*absorption) - turquoise, since red dies
  // first - along the refracted sun direction, so backlit thin crests rim
  // turquoise while thick bases stay dark. Zero cost when intensity is 0.
  float3 l = normalize(-frame.sun_direction.xyz);  // toward the sun
  float sss_intensity = frame.water_material.z;
  if (sss_intensity > 0.0) {
    // Thickness proxy: pinched crests (high crest) and lifted water are thin,
    // troughs (below rest) are thick. The absorption gain turns thickness into
    // exp() attenuation - thin crests transmit nearly white, thick bases go dark
    // and turquoise (red dies first), so the glow rims the crests.
    float height_above = input.world_pos.y - frame.water_caustics.y;
    float thickness = lerp(1.4, 0.10, crest) + saturate(-height_above) * 0.8;
    float3 trans = exp(-thickness * absorption * 4.0);  // absorption-derived turquoise
    float3 sun_travel = normalize(frame.sun_direction.xyz);
    float3 l_refr = refract(sun_travel, n, 1.0 / 1.33);  // transmitted travel dir
    if (dot(l_refr, l_refr) < 1e-6) l_refr = sun_travel;  // total-internal-reflection guard
    // Backlit lobe: the transmitted sun travels toward the camera, so the glow
    // peaks where the view direction aligns with the refracted travel direction.
    float glow = pow(saturate(dot(v, l_refr)), max(frame.water_material.w, 1.0));
    color += trans * frame.sun_color.rgb * frame.sun_direction.w * (glow * sss_intensity);
  }

  // Foam: the persistent advected field is now the primary source (whitecaps
  // streak and dissolve with the waves instead of flickering in place); the
  // instantaneous crest + shoreline terms stay as a reduced-weight high-
  // frequency sparkle. Foam is rough diffuse: it replaces the mirror response.
  float shore = saturate(1.0 - water_depth / 0.55);
  float foam_noise = WaveHeight(input.world_pos.xz * 1.7 + float2(0.0, frame.time * 0.35), frame.time) * 0.5 + 0.5;
  // Thickness model: coverage saturates with accumulated foam density.
  float persistent = 1.0 - exp(-1.1 * field.b);
  float instant = saturate(crest * smoothstep(0.6, 0.9, foam_noise) * 0.35 +
                           shore * smoothstep(0.35, 0.7, foam_noise + shore * 0.3));
  // Screen-composite so the persistent field always shows through in open water
  // (where the instantaneous term is ~0) instead of being masked by a max().
  float foam = saturate(persistent + instant - persistent * instant);
  if (foam > 0.001) {
    // Fresh foam (low age) is bright white; it greys as it ages, the noise
    // term breaking it up with high-frequency structure.
    float freshness = exp(-field.a / 6.0);
    float tone = lerp(0.55, 0.95, freshness) * (0.75 + 0.25 * foam_noise);
    float foam_ndl = max(dot(float3(0, 1, 0), l), 0.0);
    float3 foam_col = tone.xxx * (frame.sun_color.rgb * frame.sun_direction.w * (0.25 * foam_ndl) +
                                  irradiance_cube.SampleLevel(irradiance_sampler, float3(0, 1, 0), 0).rgb);
    color = lerp(color, foam_col, foam * 0.9);
    fresnel *= 1.0 - foam * 0.8;
  }

  // Sun glint: ggx on the wave normal.
  float3 h = normalize(l + v);
  float roughness = max(material.roughness_factor, 0.02);
  float a2 = roughness * roughness;
  a2 *= a2;
  float ndh = max(dot(n, h), 0.0);
  float denom = ndh * ndh * (a2 - 1.0) + 1.0;
  float distribution = a2 / max(kPi * denom * denom, 1e-6);
  float ndl = max(dot(n, l), 0.0);
  color += frame.sun_color.rgb * frame.sun_direction.w * distribution * ndl * 0.25 * fresnel;

  PsOut output;
  output.color = float4(color, 1.0);
  float2 curr = input.curr_clip.xy / input.curr_clip.w;
  float2 prev = input.prev_clip.xy / input.prev_clip.w;
  output.motion = (prev - curr) * 0.5;
  return output;
}
