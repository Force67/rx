#include "rhi_bindings.hlsli"
// Strand simulation: one thread owns one guide strand (16 verlet points). The
// groom's rest pose (the card-traced hairstyle) lives in the `rest` buffer in
// groom-local space; each frame the model matrix maps it to the world so a
// moving head bone drags the hair along. Gravity + gusty wind add life, Jakobsen
// iterations restore rest lengths, a head sphere keeps the hair off the scalp,
// and a per-point stiffness springs the strand back toward its groomed rest
// position (stiff at the root, loose at the tip) so the hairstyle holds instead
// of collapsing into straight strands.
struct HairPoint {
  float4 pos;   // xyz position, w inv_mass (0 = pinned root)
  float4 prev;  // xyz previous position, w rest length to the previous point
};

[[vk::binding(0, 0)]] RWStructuredBuffer<HairPoint> points : register(u0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<float4> rest : register(t1, space0);  // local rest, P/strand

struct SimPush {
  column_major float4x4 model;  // groom local -> world (rigid)
  float4 head;   // xyz local center, w radius
  float4 wind;   // xyz base wind, w time
  uint strand_count;
  uint points_per_strand;
  float dt;
  float damping;
  float root_stiff;
  float tip_stiff;
  float gravity;
  float pad;
};
PUSH_CONSTANTS(SimPush, pc);

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint strand = tid.x;
  if (strand >= pc.strand_count) return;
  uint base = strand * pc.points_per_strand;

  float dt = clamp(pc.dt, 0.001, 0.020);
  float3 gravity = float3(0.0, -9.8 * pc.gravity, 0.0);
  float phase = float(strand) * 0.37;
  float3 wind = pc.wind.xyz * (0.6 + 0.4 * sin(pc.wind.w * 2.1 + phase)) +
                float3(0.0, 0.12 * sin(pc.wind.w * 3.7 + phase * 1.3), 0.0);

  float3 head = mul(pc.model, float4(pc.head.xyz, 1.0)).xyz;
  float3 rest_world[16];
  for (uint r = 0; r < pc.points_per_strand; ++r) {
    rest_world[r] = mul(pc.model, float4(rest[base + r].xyz, 1.0)).xyz;
  }

  HairPoint p[16];
  for (uint i = 0; i < pc.points_per_strand; ++i) {
    p[i] = points[base + i];
    if (i == 0) {
      p[i].prev.xyz = p[i].pos.xyz;
      p[i].pos.xyz = rest_world[0];
      p[i].pos.w = 0.0;
    } else {
      float3 vel = (p[i].pos.xyz - p[i].prev.xyz) * pc.damping;
      float3 next = p[i].pos.xyz + vel + (gravity + wind) * (dt * dt);
      p[i].prev.xyz = p[i].pos.xyz;
      p[i].pos.xyz = next;
    }
  }

  for (uint iter = 0; iter < 6; ++iter) {
    for (uint i = 1; i < pc.points_per_strand; ++i) {
      float3 d = p[i].pos.xyz - p[i - 1].pos.xyz;
      float len = max(length(d), 1e-6);
      float3 dir = d / len;
      float err = len - p[i].prev.w;  // rest length rides in prev.w
      float w0 = p[i - 1].pos.w, w1 = p[i].pos.w;
      float wsum = max(w0 + w1, 1e-6);
      p[i - 1].pos.xyz += dir * (err * (w0 / wsum));
      p[i].pos.xyz -= dir * (err * (w1 / wsum));
    }
    for (uint i = 1; i < pc.points_per_strand; ++i) {
      float3 to_p = p[i].pos.xyz - head;
      float dist = length(to_p);
      if (dist < pc.head.w) p[i].pos.xyz = head + to_p * (pc.head.w / max(dist, 1e-6));
    }
  }

  // Spring back toward the groomed shape so the hairstyle holds; roots stay put,
  // tips are free to sway.
  for (uint i = 1; i < pc.points_per_strand; ++i) {
    float along = float(i) / float(pc.points_per_strand - 1);
    float s = lerp(pc.root_stiff, pc.tip_stiff, along);
    p[i].pos.xyz = lerp(p[i].pos.xyz, rest_world[i], s);
  }

  for (uint i = 0; i < pc.points_per_strand; ++i) points[base + i] = p[i];
}
