// Rasterizes a receiver into ITS OWN UV space: the clip position IS the vertex
// uv, so every texel of the decal tile is visited carrying the world position,
// normal and tangent of the surface that owns it. The pixel shader then tests
// that world position against the frame's stamp projectors. The viewport is set
// to the receiver's tile, so uv 0..1 covers exactly that tile.
//
// The skinned permutation (decal_bake_skin.vs.hlsl) poses the vertex through
// the same bone palette the scene vertex shader uses, so a splat is baked in
// the pose it actually landed on and then rides the animation.

#include "rhi_bindings.hlsli"
#include "model_transform.hlsli"

struct BakePush {
  column_major float4x4 model;
  uint first_stamp;
  uint stamp_count;
  uint skin_offset;
  uint pad;
  float2 uv_scale;  // layer uv = uv * scale + bias; identity for plain 0..1 meshes
  float2 uv_bias;
};
PUSH_CONSTANTS(BakePush, push);

// Bone palette (set 0 slot 1). Bound as a plain structured buffer rather than
// through a device address: this pass owns its layout, so it needs neither the
// SPIR-V raw loads nor the d3d12 root-SRV convention.
[[vk::binding(1, 0)]] StructuredBuffer<float4x4> bones : register(t1, space0);

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
  [[vk::location(2)]] float4 tangent : TANGENT;
  [[vk::location(3)]] float2 uv : TEXCOORD0;
#ifdef RX_SKINNED
  [[vk::location(5)]] uint4 bone_indices : BLENDINDICES;
  [[vk::location(6)]] float4 bone_weights : BLENDWEIGHT;
#endif
};

struct VsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 world_pos : TEXCOORD0;
  [[vk::location(1)]] float3 normal : NORMAL;
  [[vk::location(2)]] float4 tangent : TANGENT;
};

VsOut main(VsIn input) {
  float3 local_pos = input.position;
  float3 local_normal = input.normal;
  float3 local_tangent = input.tangent.xyz;
#ifdef RX_SKINNED
  {
    float3 posed = float3(0, 0, 0);
    float3 posed_n = float3(0, 0, 0);
    float3 posed_t = float3(0, 0, 0);
    [unroll]
    for (uint i = 0; i < 4; ++i) {
      float w = input.bone_weights[i];
      if (w <= 0.0) continue;
      float4x4 m = bones[push.skin_offset + input.bone_indices[i]];
      posed += mul(m, float4(local_pos, 1.0)).xyz * w;
      posed_n += mul((float3x3)m, local_normal) * w;
      posed_t += mul((float3x3)m, local_tangent) * w;
    }
    local_pos = posed;
    local_normal = posed_n;
    local_tangent = posed_t;
  }
#endif

  VsOut output;
  output.world_pos = mul(push.model, float4(local_pos, 1.0)).xyz;
  // A bake is persistent: a stamp baked with the old normal keeps the old
  // shading until the receiver is rebaked, so this has to match mesh.vs or a
  // rebake shifts the decal against the surface it sits on.
  float model_det;
  const float3x3 model_cof = RxCofactor((float3x3)push.model, model_det);
  const float mirror = RxMirrorSign(model_det);
  output.normal = normalize(mul(model_cof, local_normal)) * mirror;
  output.tangent = float4(mul((float3x3)push.model, local_tangent), input.tangent.w * mirror);
  // uv space IS clip space here. Depth 0 with no depth attachment bound.
  // Geometry whose mapped uv leaves 0..1 (another UDIM tile, say) lands outside
  // the tile viewport and the scissor drops it - which is exactly what the
  // forward pass does with the same transform.
  const float2 layer_uv = input.uv * push.uv_scale + push.uv_bias;
  output.sv_position = float4(layer_uv * 2.0 - 1.0, 0.0, 1.0);
  return output;
}
