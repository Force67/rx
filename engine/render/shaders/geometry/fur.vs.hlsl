#include "rhi_bindings.hlsli"
#include "model_transform.hlsli"
// Shell-based fur: the mesh is drawn as N concentric shells, each pushed out
// along the surface normal. The instance index is the shell layer; the pixel
// shader carves hair strands out of each shell with an alpha mask so the stack
// of shells reads as fur. Lengyel's classic real-time fur, on the raster path.
struct PushData {
  column_major float4x4 model;
  float3 sun_dir;     // travel direction
  float fur_length;   // total fur height in model units
  float3 sun_color;
  uint shell_count;
  float3 base_color;
  float ambient;
};
PUSH_CONSTANTS(PushData, push);

// The view projection, which alone is half the push budget.
struct FurCamera {
  column_major float4x4 view_proj;
};
[[vk::binding(0, 0)]] ConstantBuffer<FurCamera> camera : register(b0, space0);

struct VsIn {
  [[vk::location(0)]] float3 position : POSITION;
  [[vk::location(1)]] float3 normal : NORMAL;
  [[vk::location(3)]] float2 uv : TEXCOORD0;
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 normal : NORMAL;
  [[vk::location(1)]] float2 uv : TEXCOORD0;
  [[vk::location(2)]] float layer : TEXCOORD1;  // 0 at the skin, 1 at the tips
};

VsOut main(VsIn input, uint iid : SV_InstanceID) {
  float layer = float(iid) / float(max(push.shell_count - 1u, 1u));
  float3 local = input.position + input.normal * (layer * push.fur_length);
  float4 world = mul(push.model, float4(local, 1.0));
  VsOut o;
  o.pos = mul(camera.view_proj, world);
  // The shell offset above stays on input.normal in object space (it is a
  // displacement, and the matrix carries it correctly); only the shading normal
  // is a covector.
  float model_det;
  const float3x3 model_cof = RxCofactor((float3x3)push.model, model_det);
  o.normal = normalize(mul(model_cof, input.normal)) * RxMirrorSign(model_det);
  o.uv = input.uv;
  o.layer = layer;
  return o;
}
