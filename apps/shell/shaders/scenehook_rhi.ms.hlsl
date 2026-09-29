// scenehook-rhi demo mesh-shader draw path: one work group per box (dispatched
// indirectly via DrawMeshTasksIndirect with the compute-written group count).
// Each group emits an 8-vertex / 12-triangle box, a row lifted above the classic
// DrawIndirectCount boxes, sampling the same texture array in the shared pixel
// shader. Proves the mesh-task-indirect RHI path end to end.
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

struct MsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float3 color : COLOR0;
  [[vk::location(1)]] float3 normal : NORMAL0;
  [[vk::location(2)]] float2 uv : TEXCOORD0;
  [[vk::location(3)]] nointerpolation uint layer : LAYER0;
};

[outputtopology("triangle")]
[numthreads(8, 1, 1)]
void main(uint gid : SV_GroupID, uint tid : SV_GroupThreadID,
          out vertices MsOut verts[8], out indices uint3 prims[12]) {
  SetMeshOutputCounts(8, 12);

  uint idx = gid;
  float t = (float)idx - ((float)pc.count - 1.0) * 0.5;
  float3 center = float3(t * 0.7, 1.7, t * 0.5);  // row above the classic boxes
  float phase = (float)idx * 0.9 + pc.time + 1.3;
  float3 col = 0.55 + 0.45 * float3(sin(phase), sin(phase + 2.1), sin(phase + 4.2));
  uint layer = idx % pc.layer_count;

  float3 corner = kCorners[tid];
  float3 world = corner * 0.30 + center;
  float4 clip = mul(pc.view_proj, float4(world, 1.0));
  clip.xy += pc.jitter * clip.w;
  MsOut o;
  o.pos = clip;
  o.color = col;
  o.normal = normalize(corner);
  o.uv = corner.xy * 0.5 + 0.5;
  o.layer = layer;
  verts[tid] = o;

  for (uint p = tid; p < 12u; p += 8u) {
    prims[p] = uint3(kIndices[p * 3 + 0], kIndices[p * 3 + 1], kIndices[p * 3 + 2]);
  }
}
