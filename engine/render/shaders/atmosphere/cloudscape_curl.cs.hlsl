#include "rhi_bindings.hlsli"
#include "cloudscape_noise.hlsli"
// Cloudscape curl-noise field, baked once: 128^2 RG16F tiling in both axes. A
// curl field is divergence-free by construction, so displacing the erosion
// detail along it swirls the cloud edges without pumping density in or out. We
// build a tileable 2D Perlin-fbm scalar potential psi and store its rotated
// gradient (dpsi/dy, -dpsi/dx) from central differences. Because psi tiles and
// the differences wrap through the same periodic field, the curl tiles too.

[[vk::image_format("rg16f")]] [[vk::binding(0, 0)]]
RWTexture2D<float2> out_curl : register(u0, space0);

// Low-frequency swirl potential; the erosion detail this drives is already
// high-frequency, so the flow field wants to be broad.
float Potential(float2 uv) { return cs_perlin2_fbm(uv, 4.0, 4); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint2 dims;
  out_curl.GetDimensions(dims.x, dims.y);
  if (id.x >= dims.x || id.y >= dims.y) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(dims);

  // Central differences one texel apart. The potential wraps for the samples
  // that step off either edge, which keeps the field seamless at the wrap.
  float eps = 1.0 / float(dims.x);
  float px1 = Potential(uv + float2(eps, 0.0));
  float px0 = Potential(uv - float2(eps, 0.0));
  float py1 = Potential(uv + float2(0.0, eps));
  float py0 = Potential(uv - float2(0.0, eps));
  float2 curl = float2(py1 - py0, -(px1 - px0)) / (2.0 * eps);

  out_curl[id.xy] = curl * 0.5;  // roughly into [-1, 1]; the consumer scales it
}
