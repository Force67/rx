// Heightfield fluid solver (virtual pipes; Mei et al. 2007 + Chentanez & Muller
// stability kit + MAGFLOW lava). One dispatch per phase, selected by
// push.control.y, over the resolution^2 domain:
//   0 lava flux, 1 lava integrate (+thermal +solidify),
//   2 water flux, 3 water integrate (+quench).
// State is RGBA32F (r=water depth dw, g=lava depth dl, b=temperature T,
// a=crust C). Each substep runs A -> B (lava) then B -> A (water), so the
// authoritative read side is stable; the caller ping-pongs A/B per phase.
//
// Constraints this file relies on: neighbour flux is gathered only AFTER the
// flux dispatch (a barrier separates them), so the in-place flux write never
// races; the integrate pass reads neighbour depths/temps from the READ state
// and writes the WRITE state, so those reads never race either. Off-grid
// neighbours contribute zero flux (reflecting boundary).

#include "rhi_bindings.hlsli"

struct PushData {
  float4 domain;   // origin.x, origin.z, extent (m), cell size l (m)
  uint4 control;   // resolution, phase, source_count, pad
  float4 sim;      // dt_sub (s), gravity, water drag, ambient temperature
  float4 lava0;    // T_liq, T_sol, eta0, k_eta
  float4 lava1;    // yield0, k_cool, r_sol, cold drag scale
};
PUSH_CONSTANTS(PushData, push);

// rgba32f/r32f/rgba16f match the images fluid_sim.cc creates; the inferred
// format makes every store undefined per the Vulkan spec.
[[vk::image_format("rgba32f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> state_in : register(u0, space0);
[[vk::image_format("rgba32f")]] [[vk::binding(1, 0)]] RWTexture2D<float4> state_out : register(u1, space0);
[[vk::image_format("r32f")]] [[vk::binding(2, 0)]] RWTexture2D<float> bed_tex : register(u2, space0);
[[vk::image_format("rgba32f")]] [[vk::binding(3, 0)]] RWTexture2D<float4> flux_tex : register(u3, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(4, 0)]] RWTexture2D<float4> velocity_tex : register(u4, space0);

struct Source {
  float4 pos_radius;  // world x, world z, unused, radius
  float4 params;      // rate (m/s), fluid (0 water, 1 lava), temperature, unused
};
[[vk::binding(5, 0)]] StructuredBuffer<Source> sources : register(t5, space0);

// --- lava constitutive relations (temperature -> flow behaviour) ------------

// Arrhenius mobility: hot lava (T >= T_liq) flows like thick water, cooling
// lava creeps exponentially slower. m in (0, 1].
float Mobility(float T) {
  return 1.0 / (1.0 + push.lava0.z * exp(-push.lava0.w * (T - push.lava0.x)));
}

// Bingham yield head (m): only surface difference above this drives flow, so a
// cooling front stops on a slope and piles into a lobe. Rises to yield0 at the
// solidus, zero at/above the liquidus.
float YieldHead(float T) {
  float cold = saturate((push.lava0.x - T) / max(push.lava0.x - push.lava0.y, 1.0));
  return push.lava1.x * cold;
}

// Implicit drag: the water base, scaled up as lava cools (cold lava is sticky).
float LavaDrag(float T) {
  float cold = saturate((push.lava0.x - T) / max(push.lava0.x - push.lava0.y, 1.0));
  return push.sim.z * (1.0 + push.lava1.w * cold);
}

// Effective bed a fluid rides on: lava sees B + C, water sees B + C + d_lava.
float BedEffective(int2 c, bool lava) {
  float4 s = state_in[c];
  return bed_tex[c] + s.a + (lava ? 0.0 : s.g);
}
float FluidDepth(int2 c, bool lava) {
  float4 s = state_in[c];
  return lava ? s.g : s.r;
}
float Surface(int2 c, bool lava) { return BedEffective(c, lava) + FluidDepth(c, lava); }

// Smooth radial falloff (1 at the centre, 0 at the radius edge).
float SourceFalloff(float2 world, float2 centre, float radius) {
  float dist = length(world - centre);
  return 1.0 - smoothstep(0.0, max(radius, 1e-3), dist);
}

float2 CellWorld(int2 c) {
  return push.domain.xy + (float2(c) + 0.5) * push.domain.w;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint res = push.control.x;
  if (tid.x >= res || tid.y >= res) return;
  int2 id = int2(tid.xy);
  uint phase = push.control.y;
  float l = push.domain.w;
  float dt = push.sim.x;
  float g = push.sim.y;
  bool lava = (phase == 0u || phase == 1u);

  bool hasL = id.x > 0;
  bool hasR = id.x < int(res) - 1;
  bool hasB = id.y > 0;
  bool hasT = id.y < int(res) - 1;
  int2 cL = int2(max(id.x - 1, 0), id.y);
  int2 cR = int2(min(id.x + 1, int(res) - 1), id.y);
  int2 cB = int2(id.x, max(id.y - 1, 0));
  int2 cT = int2(id.x, min(id.y + 1, int(res) - 1));

  // ------------------------------------------------------------------ FLUX
  if (phase == 0u || phase == 2u) {
    float4 s = state_in[id];
    float d = lava ? s.g : s.r;
    float T = s.b;
    float m = lava ? Mobility(T) : 1.0;
    float kdrag = lava ? LavaDrag(T) : push.sim.z;
    float yield = lava ? YieldHead(T) : 0.0;
    float hc = Surface(id, lava);
    float4 f = flux_tex[id];  // previous outflow (fL, fR, fT, fB)

    // Head to each neighbour; lava's is reduced by the yield head. Off-grid
    // directions keep zero flux (reflecting boundary).
    float4 dh = float4(hc - Surface(cL, lava), hc - Surface(cR, lava),
                       hc - Surface(cT, lava), hc - Surface(cB, lava));
    if (lava) dh = sign(dh) * max(0.0, abs(dh) - yield);
    float4 gate = float4(hasL ? 1.0 : 0.0, hasR ? 1.0 : 0.0, hasT ? 1.0 : 0.0, hasB ? 1.0 : 0.0);

    // Implicit-drag flux update; A/l = l so the pipe term is dt*g*l*m*dh.
    f = max(0.0.xxxx, (f + dt * g * l * m * dh) / (1.0 + kdrag * dt)) * gate;

    // Volume clamp (Mei et al.): a cell can never export more than it holds, so
    // the explicit scheme stays non-negative and volume-conserving.
    float outflow = f.x + f.y + f.z + f.w;
    float K = min(1.0, d * l * l / (outflow * dt + 1e-9));
    flux_tex[id] = f * K;
    return;
  }

  // -------------------------------------------------------------- INTEGRATE
  float4 s = state_in[id];
  float d = lava ? s.g : s.r;
  float4 fo = flux_tex[id];  // our outflow (fL, fR, fT, fB)

  // Inflow = each neighbour's outflow toward us (its opposite component).
  float inL = hasL ? flux_tex[cL].y : 0.0;  // left neighbour's fR
  float inR = hasR ? flux_tex[cR].x : 0.0;  // right neighbour's fL
  float inB = hasB ? flux_tex[cB].z : 0.0;  // below neighbour's fT
  float inT = hasT ? flux_tex[cT].w : 0.0;  // above neighbour's fB
  float outflow = fo.x + fo.y + fo.z + fo.w;

  float dV = dt * (inL + inR + inB + inT - outflow);
  float dnew = max(0.0, d + dV / (l * l));

  // Cell velocity from through-flux, clamped (Chentanez) to 0.5*l/dt.
  float dbar = max(0.5 * (d + dnew), 1e-4);
  float u = (inL - fo.x + fo.y - inR) / (2.0 * l * dbar);
  float v = (inB - fo.w + fo.z - inT) / (2.0 * l * dbar);
  float vmax = 0.5 * l / dt;
  u = clamp(u, -vmax, vmax);
  v = clamp(v, -vmax, vmax);

  if (lava) {
    // ---- thermal: donor-cell heat advection ------------------------------
    // Each inflow carries its donor cell's temperature; the retained lava keeps
    // its own. Mass-weighted mix conserves heat where flows merge.
    float diL = dt * inL / (l * l);
    float diR = dt * inR / (l * l);
    float diB = dt * inB / (l * l);
    float diT = dt * inT / (l * l);
    float retained = max(0.0, d - dt * outflow / (l * l));
    float numer = retained * s.b + diL * state_in[cL].b + diR * state_in[cR].b +
                  diB * state_in[cB].b + diT * state_in[cT].b;
    float denom = retained + diL + diR + diB + diT;
    float T = denom > 1e-6 ? numer / denom : s.b;

    // ---- lava sources: add depth, blend temperature toward the vent -------
    float2 world = CellWorld(id);
    float add_pos = 0.0, add_neg = 0.0, heat = 0.0;
    for (uint i = 0u; i < push.control.z; ++i) {
      Source src = sources[i];
      if (src.params.y < 0.5) continue;  // water source
      float contrib = src.params.x * dt * SourceFalloff(world, src.pos_radius.xy, src.pos_radius.w);
      if (contrib >= 0.0) {
        add_pos += contrib;
        heat += contrib * src.params.z;
      } else {
        add_neg += contrib;
      }
    }
    float mixed = dnew + add_pos;
    T = mixed > 1e-6 ? (T * dnew + heat) / mixed : T;
    dnew = max(0.0, mixed + add_neg);

    // ---- cooling: radiative relaxation, faster for thin flows -------------
    T += (push.sim.w - T) * push.lava1.y * dt / max(dnew, 0.05);

    // ---- solidification: below the solidus, depth freezes into crust ------
    float C = s.a;
    if (T < push.lava0.y && dnew > 0.0) {
      float rate = push.lava1.z * saturate((push.lava0.y - T) / max(push.lava0.y, 1.0));
      float x = min(dnew, rate * dt);
      C += x;
      dnew -= x;
    }

    // Copy the water channel (owned by the water phases); update g/b/a.
    state_out[id] = float4(s.r, dnew, T, C);
    float4 vel = velocity_tex[id];
    velocity_tex[id] = float4(vel.x, vel.y, u, v);
    return;
  }

  // -------------------------------------------------------- WATER INTEGRATE
  // Edge-overshoot damp (Chentanez): suppress the ringing crest a steep
  // dam-break front leaves behind. Only genuine wet cells overshoot, and the
  // correction is clamped to REMOVE the crest (min(0, .)) so it can never mint
  // water in a dry cell straddling a bed step (a dry ridge next to deep water
  // would otherwise be pulled up to the neighbour depth). Per axis, from the
  // READ-state depths/surfaces.
  float surfC = Surface(id, false);
  float dC = s.r;
  if (dnew > 1e-3) {
    float surfL = Surface(cL, false), surfR = Surface(cR, false);
    if (surfC - surfL > 2.0 * l && surfC > surfR) {
      float dL = state_in[cL].r, dR = state_in[cR].r;
      dnew += min(0.0, 0.2 * (max(dL, 0.5 * (dC + dR)) - dnew));
    }
    float surfB = Surface(cB, false), surfT = Surface(cT, false);
    if (surfC - surfB > 2.0 * l && surfC > surfT) {
      float dB = state_in[cB].r, dTp = state_in[cT].r;
      dnew += min(0.0, 0.2 * (max(dB, 0.5 * (dC + dTp)) - dnew));
    }
  }
  dnew = max(0.0, dnew);

  // Water sources.
  float2 world = CellWorld(id);
  for (uint i = 0u; i < push.control.z; ++i) {
    Source src = sources[i];
    if (src.params.y >= 0.5) continue;  // lava source
    dnew += src.params.x * dt * SourceFalloff(world, src.pos_radius.xy, src.pos_radius.w);
  }
  dnew = max(0.0, dnew);

  // Quench where hot lava and water meet: some lava freezes to crust, some
  // water boils off. Phase 3 owns the full RGBA write, so editing g/a here is
  // race-free. dl/C/T come from the (post-lava) READ state.
  float dl = s.g;
  float crust = s.a;
  if (dnew > 0.01 && dl > 0.01) {
    float q = min(dl, 0.5 * dt);
    dl -= q;
    crust += q;
    dnew -= min(dnew, 0.25 * q);
  }

  state_out[id] = float4(dnew, dl, s.b, crust);
  float4 vel = velocity_tex[id];
  velocity_tex[id] = float4(u, v, vel.z, vel.w);
}
