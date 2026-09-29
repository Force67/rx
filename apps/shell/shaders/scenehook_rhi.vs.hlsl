// scenehook-rhi demo draw pass (classic DrawIndirectCount path): instanced unit
// boxes from SV_VertexID, per-instance world offset / colour / texture-array
// layer read from the compute-written buffer-device-address arena.
struct Push {
  column_major float4x4 view_proj;
  uint64_t instance_addr;
  uint64_t args_addr;
  uint64_t count_addr;
  uint64_t churn_addr;
  float2 jitter;
  uint count;
  float time;
  uint layer_count;
  uint pad;
};
[[vk::push_constant]] Push pc;

static const float3 kCorners[8] = {
  float3(-1, -1, -1), float3(1, -1, -1), float3(1, 1, -1), float3(-1, 1, -1),
  float3(-1, -1, 1),  float3(1, -1, 1),  float3(1, 1, 1),  float3(-1, 1, 1),
};
static const uint kIndices[36] = {
  0, 1, 2, 0, 2, 3,  5, 4, 7, 5, 7, 6,  4, 0, 3, 4, 3, 7,
  1, 5, 6, 1, 6, 2,  3, 2, 6, 3, 6, 7,  4, 5, 1, 4, 1, 0,
};

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 color : COLOR0;
  [[vk::location(1)]] float3 normal : NORMAL0;
  [[vk::location(2)]] float2 uv : TEXCOORD0;
  [[vk::location(3)]] nointerpolation uint layer : LAYER0;
};

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  uint64_t base = pc.instance_addr + (uint64_t)iid * 48u;
  float4 offset = vk::RawBufferLoad<float4>(base);       // xyz centre, w half-size
  float4 color = vk::RawBufferLoad<float4>(base + 16u);
  uint layer = vk::RawBufferLoad<uint>(base + 32u);
  float3 corner = kCorners[kIndices[vid]];
  float3 world = corner * offset.w + offset.xyz;
  float4 clip = mul(pc.view_proj, float4(world, 1.0));
  clip.xy += pc.jitter * clip.w;
  VsOut o;
  o.pos = clip;
  o.color = color.rgb;
  o.normal = normalize(corner);
  o.uv = corner.xy * 0.5 + 0.5;
  o.layer = layer;
  return o;
}
