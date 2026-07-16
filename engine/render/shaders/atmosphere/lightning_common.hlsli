// Shared by the lightning bolt shaders (lightning_bolt.vs/.ps): the push
// block and the deterministic stroke envelope. The bolt is a pure function of
// (strike_seed, strike_pos, SV_InstanceID) - no CPU geometry buffers - and the
// envelope here is mirrored bit-for-bit by LightningSystem::Envelope on the
// CPU so the positioned flash light and the demo/game's global flash scalar
// flicker in lockstep with the rendered channel.
#ifndef RX_LIGHTNING_COMMON_HLSLI_
#define RX_LIGHTNING_COMMON_HLSLI_

// PcgHash / Hash4 / kQuadCorners / kTau.
#include "precip_common.hlsli"

struct LightningPush {
  column_major float4x4 view_proj;
  float3 cam_pos;    float time;    // seconds
  float3 strike_pos; float age;     // seconds since the strike; < 0 = none
  uint seed; float energy; float2 jitter;  // jitter in ndc units
};

static const float kStrikeDuration = 0.45;

// Instant attack decaying over ~80 ms, then one or two re-strokes (hashed
// timing/amplitude off the seed) inside the window - the classic flicker.
// Matches LightningSystem::Envelope exactly.
float StrikeEnvelope(float age, uint seed) {
  if (age < 0.0 || age >= kStrikeDuration) return 0.0;
  float4 h = Hash4(seed * 747796405u + 3u);
  float env = exp(-age * 26.0);
  float t1 = 0.10 + 0.08 * h.x;
  if (age > t1) env += (0.5 + 0.4 * h.y) * exp(-(age - t1) * 30.0);
  if (h.z > 0.35) {
    float t2 = 0.22 + 0.10 * h.w;
    if (age > t2) env += (0.35 + 0.30 * h.x) * exp(-(age - t2) * 30.0);
  }
  // Hard-fade the tail so nothing pops when the window closes.
  return env * saturate((kStrikeDuration - age) * 20.0);
}

#endif  // RX_LIGHTNING_COMMON_HLSLI_
