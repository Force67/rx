// Persistent water-surface field, one nested ring per dispatch. Phase 0
// recenters/advects/decays the previous frame's ring and steps the ripple wave
// equation (ring 0); phase 1 injects crest foam + object disturbances. The ring
// is fully resampled from the previous texture each frame (bilinear, keyed off
// the OLD snapped origin), so there is no toroidal bookkeeping and the field
// never swims under the camera. Channels: R ripple height, G ripple velocity,
// B foam density, A foam age (seconds).

#include "rhi_bindings.hlsli"

struct PushData {
  float4 origin;       // new origin xz, half_extent, texel_world
  float4 prev_origin;  // old origin xz (advection source frame)
  float4 drift_time;   // wave-drift xz (m/s), dt, time
  uint4 control;       // ring index, phase, fft-ocean flag, disturbance count
};
[[vk::push_constant]] ConstantBuffer<PushData> push : register(b0, space0);

// Previous frame's ring (sampled at the old origin) and this frame's target.
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] Texture2D<float4> prev_ring : register(t0, space0);
[[vk::combinedImageSampler]] [[vk::binding(0, 0)]] SamplerState prev_sampler : register(s0, space0);
[[vk::binding(1, 0)]] RWTexture2D<float4> cur_ring : register(u1, space0);
// FFT normal/foam map (.w foam), wrap-sampled over the 64 m ocean tile.
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] Texture2D<float4> ocean_foam : register(t2, space0);
[[vk::combinedImageSampler]] [[vk::binding(2, 0)]] SamplerState ocean_sampler : register(s2, space0);

struct Disturbance {
  float4 pos_radius;  // xyz world, w radius
  float4 params;      // ripple_strength, foam_amount, vel_x, vel_z
};
[[vk::binding(3, 0)]] StructuredBuffer<Disturbance> disturbances : register(t3, space0);

static const uint kSize = 512u;
static const float kOceanPatchSize = 64.0;  // mirrors OceanFft::kPatchSize
static const float kFoamHalfLife = 9.0;     // seconds until foam density halves
static const float kRippleSpeed = 4.0;      // ripple phase speed (m/s)
static const float kRippleDamp = 0.994;     // per-step ripple energy retention

float2 TexelWorld(uint2 id) {
  float2 uv = (float2(id) + 0.5) / float(kSize);
  return push.origin.xy + (uv - 0.5) * (2.0 * push.origin.z);
}

// world XZ -> uv in the previous ring; xy = uv, valid only inside the unit square.
float3 PrevUv(float2 world) {
  float2 uv = (world - push.prev_origin.xy) / (2.0 * push.origin.z) + 0.5;
  float inside = (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0) ? 1.0 : 0.0;
  return float3(uv, inside);
}

float4 SamplePrev(float2 world) {
  float3 uv = PrevUv(world);
  return prev_ring.SampleLevel(prev_sampler, uv.xy, 0.0) * uv.z;  // empty outside
}

// Analytic Gerstner crest proxy: same two dominant directions the fallback
// water field uses, so foam lands on the same moving wave tops when the FFT
// ocean is off.
float GerstnerCrest(float2 world, float t) {
  float c = 0.0;
  c += max(sin(dot(world, float2(0.780869, 0.624695)) * 0.33 + t * 1.15), 0.0);
  c += max(sin(dot(world, float2(-0.286206, -0.958164)) * 0.57 + t * 0.90), 0.0);
  return saturate(c * 0.5);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  if (tid.x >= kSize || tid.y >= kSize) return;
  uint2 id = tid.xy;
  float2 world = TexelWorld(id);
  float dt = push.drift_time.z;
  uint ring = push.control.x;
  uint phase = push.control.y;

  if (phase == 0u) {
    // --- recenter + advect + decay + ripple step -----------------------------
    float texel = push.origin.z * 2.0 / float(kSize);

    // Foam advects with the wave drift: back-sample where this water came from.
    float2 from = world - push.drift_time.xy * dt;
    float4 carried = SamplePrev(from);
    float foam = carried.b;
    float age = carried.a;

    // Exponential foam decay; age only accrues where foam actually persists.
    float decay = exp2(-dt / kFoamHalfLife);
    foam *= decay;
    age = foam > 1e-3 ? age + dt : 0.0;

    // Ripple wave equation on ring 0 only (near-camera detail). Neighbours are
    // read from the previous texture, so the in-place write below never races.
    float height = 0.0, vel = 0.0;
    if (ring == 0u) {
      float4 here = SamplePrev(world);
      float hc = here.r;
      float hl = SamplePrev(world - float2(texel, 0.0)).r;
      float hr = SamplePrev(world + float2(texel, 0.0)).r;
      float hd = SamplePrev(world - float2(0.0, texel)).r;
      float hu = SamplePrev(world + float2(0.0, texel)).r;
      float lap = (hl + hr + hd + hu - 4.0 * hc) / max(texel * texel, 1e-4);
      vel = (here.g + kRippleSpeed * kRippleSpeed * lap * dt) * kRippleDamp;
      height = hc + vel * dt;
      // Stability clamp: a stray impulse must not detonate the field.
      height = clamp(height, -2.0, 2.0);
      vel = clamp(vel, -8.0, 8.0);
    }
    cur_ring[id] = float4(height, vel, foam, age);
    return;
  }

  // --- injection (in-place, no neighbour reads) ------------------------------
  float4 cell = cur_ring[id];
  float height = cell.r, vel = cell.g, foam = cell.b, age = cell.a;

  // Crest foam from the wave source at this texel's world position. The FFT
  // fold-foam channel (when the FFT ocean is on) captures hard whitecaps; a
  // gentler analytic Gerstner-crest term always adds moving wave-top foam so
  // even a calm sea shows persistent, advecting streaks rather than nothing.
  // All injections are per-second rates times dt: foam accumulates over the
  // ~13 s foam time constant, so a small rate reaches a modest steady density
  // rather than saturating (density = rate * ~13).
  float fft_foam = (push.control.z != 0u)
                       ? ocean_foam.SampleLevel(ocean_sampler, world / kOceanPatchSize, 0.0).w
                       : 0.0;
  float gerstner = GerstnerCrest(world, push.drift_time.w);
  float inject = (saturate((fft_foam - 0.12) * 1.5) * 0.7 +
                  saturate((gerstner - 0.72) * 2.5) * 0.5) * dt;

  // Object disturbances: a wake ripple (ring 0) plus a foam splat, both faded
  // toward the disturbance radius.
  for (uint i = 0u; i < push.control.w; ++i) {
    Disturbance d = disturbances[i];
    float dist = length(world - d.pos_radius.xz);
    float falloff = 1.0 - saturate(dist / max(d.pos_radius.w, 0.01));
    falloff *= falloff;  // soft edge
    if (falloff <= 0.0) continue;
    if (ring == 0u) vel += d.params.x * falloff * dt * 12.0;
    inject += d.params.y * falloff * dt;
  }

  // Fresh foam is age 0: mass-weight the running age toward zero as we add.
  float new_foam = min(foam + inject, 2.0);
  age = new_foam > 1e-3 ? foam * age / new_foam : 0.0;
  height = clamp(height, -2.0, 2.0);
  vel = clamp(vel, -8.0, 8.0);
  cur_ring[id] = float4(height, vel, new_foam, age);
}
