#include "rhi_bindings.hlsli"
// Tessendorf spectrum evolution: h(k,t) = h0(k) e^{iwt} + h0*(-k) e^{-iwt}
// with deep-water dispersion w = sqrt(g|k|). Also emits the choppy
// displacement spectra -i k/|k| h for x and z. Everything stays in frequency
// space; the FFT passes bring it back to the spatial maps.
struct SpectrumPush {
  uint size;
  float patch_size;
  float time;
  float pad0;
};
PUSH_CONSTANTS(SpectrumPush, pc);

[[vk::binding(0, 0)]] Texture2D<float4> h0_map : register(t0, space0);
[[vk::binding(1, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> spec_hx : register(u1, space0);
[[vk::binding(2, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> spec_z : register(u2, space0);

static const float kPi = 3.14159265;

float2 CMul(float2 a, float2 b) { return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= pc.size || id.y >= pc.size) return;
  float n = float(id.x) - float(pc.size) * 0.5;
  float m = float(id.y) - float(pc.size) * 0.5;
  float2 k = 2.0 * kPi * float2(n, m) / pc.patch_size;
  float klen = max(length(k), 1e-5);

  float4 h0 = h0_map.Load(int3(id.xy, 0));
  float w = sqrt(9.81 * klen);
  float2 e = float2(cos(w * pc.time), sin(w * pc.time));
  float2 e_conj = float2(e.x, -e.y);
  // h = h0 e^{iwt} + conj(h0(-k)) e^{-iwt}
  float2 h = CMul(h0.xy, e) + CMul(float2(h0.z, -h0.w), e_conj);
  // Choppy displacement: -i k_hat h  ( (-i)(a+ib) = b - ia ).
  float2 kn = k / klen;
  float2 dx = kn.x * float2(h.y, -h.x);
  float2 dz = kn.y * float2(h.y, -h.x);

  spec_hx[id.xy] = float4(h, dx);
  spec_z[id.xy] = float4(dz, 0.0, 0.0);
}
