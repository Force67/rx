// Persistent water-surface field, one nested ring per dispatch. Phase 0
// recenters/advects/decays the previous frame's ring and steps the ripple wave
// equation (ring 0); phase 1 injects crest foam + object disturbances. The ring
// is fully resampled from the previous texture each frame (bilinear, keyed off
// the OLD snapped origin), so there is no toroidal bookkeeping and the field
// never swims under the camera. Channels: R ripple height, G ripple velocity,
// B foam density, A foam age (seconds).
//
// Local interaction (control.z flags): the DEPTH path projects each ring-0
// texel's water-plane column into the frame, samples the opaque prepass depth,
// reconstructs the geometry's world position and — where geometry crosses the
// waterline right at this column — stores a soft intersection band in a small
// ping-ponged mask texture; the per-frame CHANGE of that band drives a ripple
// impulse + foam, so ANY geometry breaking the surface ripples with no CPU
// disturbances (a still object holds a steady band and stays quiet). The
// OBSTACLE path treats texels whose analytic terrain height sits above the
// water as reflecting Neumann boundaries: their ripple state is zeroed and the
// wave stencil reads the centre's own value in place of an obstacle neighbour,
// so rings bounce off the beach instead of crossing it.

#include "rhi_bindings.hlsli"

struct PushData {
  float4 origin;       // new origin xz, half_extent, texel_world
  float4 prev_origin;  // old origin xz (advection source frame), unused zw
  float4 drift_time;   // wave-drift xz (m/s), dt, time
  uint4 control;       // ring index, phase, flags (bit0 fft,bit1 interact,bit2 obstacle), disturbance count
  column_major float4x4 view_proj;      // world -> clip, projecting the texel column to screen
  column_major float4x4 inv_view_proj;  // clip -> world, reconstructing geometry from depth
  float4 island;       // analytic beach: center xz, gaussian sigma (m), peak (m)
  float4 idepth0;      // standing-displacement depth (m), waterline band (m), foam scale, water rest level
  float4 idepth1;      // render width, render height, xz proximity (m), ripple gain
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
  float2 wake;        // elongation (speed stretch), angular_velocity (turn skew)
};
[[vk::binding(3, 0)]] StructuredBuffer<Disturbance> disturbances : register(t3, space0);

// Opaque prepass depth (reversed z, R32F): the interaction path samples it to
// find geometry crossing the waterline. Always bound (a valid graph resource);
// only read when the interact flag is set.
[[vk::binding(4, 0)]] Texture2D<float> opaque_depth : register(t4, space0);
// Last/this frame's ring-0 intersection band (R32F, GENERAL), ping-ponged with
// the rings so it recenters with the camera. Only touched on ring 0 interact.
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2D<float> prev_mask : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState prev_mask_sampler : register(s5, space0);
[[vk::binding(6, 0)]] RWTexture2D<float> cur_mask : register(u6, space0);
// FFT ocean displacement (.y world height), wrap-sampled over the ocean tile,
// so the waterline the interaction band tests against rides the real swell:
// passing waves keep the band moving even under a perfectly still floater, so
// the surface stays alive at steady state instead of going glassy-quiet. Bound
// to the ring itself (a harmless placeholder) and the analytic path used when
// the FFT ocean is off.
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]] Texture2D<float4> ocean_disp : register(t7, space0);
[[vk::combinedImageSampler]] [[vk::binding(7, 0)]] SamplerState ocean_disp_sampler : register(s7, space0);

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

float SamplePrevMask(float2 world) {
  float3 uv = PrevUv(world);
  return prev_mask.SampleLevel(prev_mask_sampler, uv.xy, 0.0) * uv.z;
}

// Analytic beach height, mirroring shore_wetting.cs.hlsl's TerrainHeight so the
// interaction boundary and the shoreline wetting agree on where the sand is: a
// radial gaussian dome peaking `peak` above rest water at the centre, dipping to
// -peak far out. A captured heightmap would replace this in a real world.
float TerrainHeight(float2 world) {
  float2 d = world - push.island.xy;
  float sigma = max(push.island.z, 0.1);
  float g = exp(-dot(d, d) / (2.0 * sigma * sigma));
  return push.island.w * (2.0 * g - 1.0);
}

// A texel is an obstacle (reflecting boundary) when the obstacle flag is set and
// the sand there rises above the rest water level.
bool IsObstacle(float2 world) {
  return (push.control.z & 4u) != 0u && TerrainHeight(world) > push.idepth0.w;
}

// Water surface height the interaction band tests against. Rides the FFT swell
// when the ocean is on (so waves lap up/down a static floater and keep it
// ringing); a small analytic proxy otherwise. Rest level added so a non-zero
// water plane still works.
float WaterSurfaceHeight(float2 world) {
  if ((push.control.z & 1u) != 0u)
    return push.idepth0.w + ocean_disp.SampleLevel(ocean_disp_sampler, world / kOceanPatchSize, 0.0).y;
  float t = push.drift_time.w;
  float h = 0.16 * sin(dot(world, float2(0.780869, 0.624695)) * 0.33 + t * 1.15) +
            0.06 * sin(dot(world, float2(-0.286206, -0.958164)) * 0.57 + t * 0.90);
  return push.idepth0.w + h;
}

// Vertical velocity of the analytic swell (d/dt of the height terms above). The
// waterline intersection at a floater's footprint is a stable presence band
// (its vertical faces span every height), so its ripple has to come from the
// water MOVING past it: this drives a bob impulse over the band. Zero-mean over
// a wave cycle, so a generous gain never accumulates a DC drift. It is analytic
// even under the FFT ocean (a plausible-phase proxy, only setting the impulse
// magnitude, not the shading surface).
float SwellVerticalVelocity(float2 world) {
  float t = push.drift_time.w;
  return 0.16 * 1.15 * cos(dot(world, float2(0.780869, 0.624695)) * 0.33 + t * 1.15) +
         0.06 * 0.90 * cos(dot(world, float2(-0.286206, -0.958164)) * 0.57 + t * 0.90);
}

// Reflecting Neumann neighbour: an obstacle neighbour contributes the centre's
// own height, so the wave sees a zero-gradient wall and bounces instead of
// leaking energy into the beach.
float NeighborHeight(float2 nworld, float hc) {
  return IsObstacle(nworld) ? hc : SamplePrev(nworld).r;
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
    // Obstacle neighbours reflect (Neumann); an obstacle texel keeps no ripple.
    float height = 0.0, vel = 0.0;
    if (ring == 0u) {
      if (IsObstacle(world)) {
        cur_ring[id] = float4(0.0, 0.0, foam, age);
        return;
      }
      float4 here = SamplePrev(world);
      float hc = here.r;
      float hl = NeighborHeight(world - float2(texel, 0.0), hc);
      float hr = NeighborHeight(world + float2(texel, 0.0), hc);
      float hd = NeighborHeight(world - float2(0.0, texel), hc);
      float hu = NeighborHeight(world + float2(0.0, texel), hc);
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
  float fft_foam = (push.control.z & 1u)
                       ? ocean_foam.SampleLevel(ocean_sampler, world / kOceanPatchSize, 0.0).w
                       : 0.0;
  float gerstner = GerstnerCrest(world, push.drift_time.w);
  // Rates tuned against the 20 s equilibrium (density ~ rate * 13 * duty
  // cycle): only genuinely pinched crests inject, or the whole sea saturates
  // into milky bands by the time the field reaches steady state.
  float inject = (saturate((fft_foam - 0.18) * 1.5) * 0.5 +
                  saturate((gerstner - 0.80) * 3.0) * 0.2) * dt;

  // Object disturbances: a VELOCITY-SHAPED wake ripple (ring 0) plus a foam
  // splat. A still body (speed ~0) collapses to the old radial blob; a moving
  // body stretches the splat along its heading, sharpens the leading (bow) edge
  // and trails a stern band, skewing sideways under a turn. Speed scales the
  // stretch (via the CPU-supplied elongation) and the intensity.
  for (uint i = 0u; i < push.control.w; ++i) {
    Disturbance d = disturbances[i];
    float2 rel = world - d.pos_radius.xz;
    float radius = max(d.pos_radius.w, 0.01);
    float2 vel_xz = d.params.zw;
    float speed = length(vel_xz);
    float2 fwd = speed > 1e-3 ? vel_xz / speed : float2(0.0, 1.0);
    float2 side = float2(-fwd.y, fwd.x);
    float along = dot(rel, fwd);    // + ahead of the body (bow), - behind (stern)
    float across = dot(rel, side);
    float e = d.wake.x;             // elongation (0 = radial)
    // Turning skews the wake: shear the lateral offset by the yaw rate along the
    // motion axis, so the wake curves to the inside of the turn.
    across += d.wake.y * along * 0.4;
    // Anisotropic footprint: a tight bow ahead, a long tail astern, narrower
    // across as it stretches. e = 0 makes every scale = radius (a plain circle).
    float fore = radius / (1.0 + e * 1.5);         // ahead: compressed, sharp bow
    float aft = radius * (1.0 + e * 3.0);          // behind: elongated stern tail
    float lat = radius * (0.6 + 0.4 / (1.0 + e));  // across: narrows with speed
    float an = along >= 0.0 ? along / fore : along / aft;
    float ac = across / lat;
    float falloff = 1.0 - saturate(sqrt(an * an + ac * ac));
    falloff *= falloff;  // soft edge
    if (falloff <= 0.0) continue;
    float bow = saturate(an);    // 0 astern .. 1 at the bow tip
    float stern = saturate(-an); // 0 at the bow .. 1 astern
    // Ripple is strongest right at the bow (the pressure wave a hull pushes);
    // foam lingers in the trailing stern band.
    if (ring == 0u) vel += d.params.x * falloff * (1.0 + 1.6 * bow * e) * dt * 12.0;
    inject += d.params.y * falloff * (1.0 + 1.0 * stern) * dt;
  }

  // Depth-buffer interaction: turn geometry crossing the waterline at this
  // ring-0 column into a ripple impulse + foam. The mask stores a soft band
  // that peaks where geometry meets the water; the per-frame change of that
  // band forces the wave (so a bobbing object ripples, a static one holds a
  // steady band and stays quiet), and rising bands throw a little foam.
  if (ring == 0u && (push.control.z & 2u) != 0u && !IsObstacle(world)) {
    float surface = WaterSurfaceHeight(world);  // rides the swell
    float band = max(push.idepth0.y, 0.01);
    float prevm = SamplePrevMask(world);
    float mask_now = 0.0;

    float3 col = float3(world.x, surface, world.y);
    float4 clip = mul(push.view_proj, float4(col, 1.0));
    if (clip.w > 1e-4) {
      float2 suv = (clip.xy / clip.w) * 0.5 + 0.5;
      if (all(suv >= 0.0) && all(suv <= 1.0)) {
        int2 px = int2(suv * push.idepth1.xy);
        float d = opaque_depth.Load(int3(px, 0));
        if (d > 0.0) {  // not sky
          float2 ndc = suv * 2.0 - 1.0;
          float4 wh = mul(push.inv_view_proj, float4(ndc, d, 1.0));
          float3 gworld = wh.xyz / wh.w;
          float dv = abs(gworld.y - surface);
          float dxz = length(gworld.xz - world);
          // Geometry that both straddles the waterline AND sits at this column
          // (not a distant wall seen past the water) is a genuine intersection.
          if (dv < band && dxz < push.idepth1.z) mask_now = saturate(1.0 - dv / band);
        }
        // On-screen but sky/away-from-waterline: the band is genuinely gone.
      } else {
        mask_now = prevm;  // off-screen: hold state, no injection
      }
    } else {
      mask_now = prevm;  // behind camera: hold state, no injection
    }

    float delta = mask_now - prevm;
    // Swell-driven bob over the presence band (keeps a static floater ringing)
    // plus a sharper impulse from the band actually shifting (moving geometry).
    // Both are zero-mean, so the velocity injection carries no DC drift.
    float dsdt = SwellVerticalVelocity(world);
    vel += push.idepth1.w * (mask_now * dsdt + delta);
    // Persistent, BOUNDED waterline displacement: softly pin the surface toward
    // a small dent under the footprint. This is a spring toward a fixed target
    // (kInteractStandDisp * mask), so it CANNOT accumulate — the previous agent's
    // runaway came from integrating an un-signed source; this can't. The wave
    // equation rings this dent outward, which is what makes the intersection
    // visibly ripple even at steady state.
    float target = -push.idepth0.x * mask_now;
    height = lerp(height, target, saturate(0.35 * mask_now));
    // Foam scaled by how fast the intersection moves (the swell washing the
    // waterline), as a per-second RATE * dt so it integrates to a modest steady
    // density over the ~13 s foam tau instead of saturating into milky bands —
    // the whole shoreline is a large, always-active intersection.
    inject += push.idepth0.z * mask_now * abs(dsdt) * dt;
    cur_mask[id] = mask_now;
  }

  // Fresh foam is age 0: mass-weight the running age toward zero as we add.
  float new_foam = min(foam + inject, 2.0);
  age = new_foam > 1e-3 ? foam * age / new_foam : 0.0;
  height = clamp(height, -2.0, 2.0);
  vel = clamp(vel, -8.0, 8.0);
  cur_ring[id] = float4(height, vel, new_foam, age);
}
