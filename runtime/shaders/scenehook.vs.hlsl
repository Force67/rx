// Scene-hook demo draw pass: instanced unit boxes generated from SV_VertexID,
// each instance's world offset + colour read from the compute-written buffer-
// device-address arena. Mirrors an app's own GPU-driven draw reading a BDA quad
// arena.
struct Push {
  column_major float4x4 view_proj;  // rx's exact unjittered view*proj this frame
  uint64_t addr;                    // device address of the instance arena
  float2 jitter;                    // rx TAA jitter, ndc units
  uint count;
  float time;
};
[[vk::push_constant]] Push pc;

static const float3 kCorners[8] = {
  float3(-1, -1, -1), float3(1, -1, -1), float3(1, 1, -1), float3(-1, 1, -1),
  float3(-1, -1, 1),  float3(1, -1, 1),  float3(1, 1, 1),  float3(-1, 1, 1),
};
// 12 triangles, outward-facing.
static const uint kIndices[36] = {
  0, 1, 2, 0, 2, 3,  // -z
  5, 4, 7, 5, 7, 6,  // +z
  4, 0, 3, 4, 3, 7,  // -x
  1, 5, 6, 1, 6, 2,  // +x
  3, 2, 6, 3, 6, 7,  // +y
  4, 5, 1, 4, 1, 0,  // -y
};

struct VsOut {
  float4 pos : SV_Position;
  float3 color : COLOR0;
  float3 normal : NORMAL0;
};

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  uint64_t base = pc.addr + (uint64_t)iid * 32u;
  float4 offset = vk::RawBufferLoad<float4>(base);       // xyz centre, w half-size
  float4 color = vk::RawBufferLoad<float4>(base + 16u);  // rgb colour
  float3 corner = kCorners[kIndices[vid]];
  float3 world = corner * offset.w + offset.xyz;
  float4 clip = mul(pc.view_proj, float4(world, 1.0));
  clip.xy += pc.jitter * clip.w;  // stay pixel-aligned with rx geometry under TAA
  VsOut o;
  o.pos = clip;
  o.color = color.rgb;
  o.normal = normalize(corner);
  return o;
}
