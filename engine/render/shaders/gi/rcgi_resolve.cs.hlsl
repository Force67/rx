#include "rhi_bindings.hlsli"
#include "gi/rcgi_common.hlsli"
// RCGI M1 debug composite (the temporary filler M2 replaces with the gather /
// denoise / upscale chain). Per pixel: reconstruct the world position from the
// prepass depth, decode the prepass normal, sample the irradiance cascades, and
// write the full-res indirect-diffuse irradiance the forward pass folds in. The
// output matches the texture M2 will produce (same name/format/slot).

[[vk::image_format("rgba16f")]] [[vk::binding(0, 0)]] RWTexture2D<float4> irradiance_out : register(u0, space0);
[[vk::binding(1, 0)]] Texture2D<float> depth_map : register(t1, space0);
[[vk::binding(2, 0)]] Texture2D<float2> normal_map : register(t2, space0);
[[vk::binding(3, 0)]] ConstantBuffer<RcgiGlobals> rcgi : register(b3, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] Texture2D rcgi_irr_atlas : register(t4, space0);
[[vk::combinedImageSampler]] [[vk::binding(4, 0)]] SamplerState rcgi_irr_sampler : register(s4, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] Texture2D rcgi_vis_atlas : register(t5, space0);
[[vk::combinedImageSampler]] [[vk::binding(5, 0)]] SamplerState rcgi_vis_sampler : register(s5, space0);

struct PushData {
  column_major float4x4 inv_view_proj;  // unjittered
  float2 inv_size;
  float2 pad;
  float4 camera_pos;  // xyz eye, w intensity
};
PUSH_CONSTANTS(PushData, push);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint width, height;
  irradiance_out.GetDimensions(width, height);
  if (id.x >= width || id.y >= height) return;

  int3 p = int3(id.xy, 0);
  float depth = depth_map.Load(p);
  if (depth <= 0.0) {  // sky
    irradiance_out[id.xy] = 0.0.xxxx;
    return;
  }

  float2 uv = (float2(id.xy) + 0.5) * push.inv_size;
  float4 world = mul(push.inv_view_proj, float4(uv * 2.0 - 1.0, depth, 1.0));
  float3 pos = world.xyz / world.w;
  float3 n = RcgiOctDecode(normal_map.Load(p).rg);
  float3 v = normalize(push.camera_pos.xyz - pos);

  float3 irr = SampleRcgiIrradiance(rcgi, rcgi_irr_atlas, rcgi_irr_sampler, rcgi_vis_atlas,
                                    rcgi_vis_sampler, pos, n, v) * push.camera_pos.w;
  irradiance_out[id.xy] = float4(irr, 1.0);
}
