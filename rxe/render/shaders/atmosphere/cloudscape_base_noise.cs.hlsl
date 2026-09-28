#include "rhi_bindings.hlsli"
#include "cloudscape_noise.hlsli"
// Cloudscape base shape volume, baked once: 128^3 RGBA8 tiling in all three
// axes.
//   R = "perlin-worley": low-frequency Perlin fbm dilated by an inverted-Worley
//       fbm, which pushes the smooth Perlin body out toward the billowing
//       cellular silhouette the raymarcher reads as the cloud base.
//   G,B,A = inverted-Worley fbm at rising base frequencies, the erosion detail
//       carved off the shape at three scales.
// Every generator wraps its lattice modulo the octave period, so the volume is
// seamless where it repeats across the sky.

[[vk::image_format("rgba8")]] [[vk::binding(0, 0)]]
RWTexture3D<float4> out_noise : register(u0, space0);

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID) {
  uint3 dims;
  out_noise.GetDimensions(dims.x, dims.y, dims.z);
  if (any(id >= dims)) return;
  float3 uvw = (float3(id) + 0.5) / float3(dims);

  float perlin = cs_perlin3_fbm(uvw, 4.0, 4);
  float worley6 = cs_worley3_fbm(uvw, 6.0);
  // Dilate the Perlin body by the coarse Worley fbm; the remap stretches the
  // Perlin range so the cellular pattern eats into it rather than just scaling.
  float pw = saturate(cs_remap(perlin, worley6 - 1.0, 1.0, 0.0, 1.0));

  float g = worley6;
  float b = cs_worley3_fbm(uvw, 12.0);
  float a = cs_worley3_fbm(uvw, 24.0);
  out_noise[id] = float4(pw, g, b, a);
}
