#include "rhi_bindings.hlsli"
// Froxel volumetric lighting, pass 2: front-to-back integration. Each column
// accumulates energy-conserving inscatter and transmittance so any consumer
// (the fog composite, translucents, particles) can sample its depth's slice
// and get "everything in front of me" in one fetch.
struct IntegratePush {
  float near_plane;
  float far_plane;
  uint slices;
  float pad;
};
PUSH_CONSTANTS(IntegratePush, pc);

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture3D<float4> integrated : register(u0, space0);
[[vk::image_format("rgba16f")]] [[vk::binding(1, 0)]] RWTexture3D<float4> scatter : register(u1, space0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint3 dims;
  integrated.GetDimensions(dims.x, dims.y, dims.z);
  if (any(id.xy >= dims.xy)) return;

  float3 accum = float3(0.0, 0.0, 0.0);
  float transmittance = 1.0;
  float prev_depth = pc.near_plane;
  for (uint z = 0; z < pc.slices; ++z) {
    float slice_end =
        pc.near_plane * pow(pc.far_plane / pc.near_plane, float(z + 1u) / float(pc.slices));
    float dz = slice_end - prev_depth;
    prev_depth = slice_end;

    float4 s = scatter[uint3(id.xy, z)];
    float extinction = max(s.a, 1e-5);
    float step_t = exp(-extinction * dz);
    // Analytic integral of a constant source over the step.
    accum += transmittance * (s.rgb / extinction) * (1.0 - step_t);
    transmittance *= step_t;
    integrated[uint3(id.xy, z)] = float4(accum, transmittance);
  }
}
