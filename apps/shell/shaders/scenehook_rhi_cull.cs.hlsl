// scenehook-rhi demo: GPU-driven "cull/placement" compute pass, expressed in
// pure RHI. Thread i writes instance i's transform+colour+array-layer into a
// buffer-device-address arena; thread 0 also writes the indirect DRAW args, the
// draw-count, and the mesh-task-dispatch args into their own BDA buffers, which
// the graphics pass then consumes through DrawIndirectCount / DrawMeshTasks-
// Indirect. Proves compute-writes-its-own-draw-args running on rx's frame list.
struct Push {
  column_major float4x4 view_proj;
  uint64_t instance_addr;  // arena base
  uint64_t args_addr;      // indirect args (draw at 0, mesh-task at 16)
  uint64_t count_addr;     // draw count (u32)
  uint64_t churn_addr;     // per-frame scratch retired via deferred destruction
  float2 jitter;
  uint count;
  float time;
  uint layer_count;
  uint pad;
};
[[vk::push_constant]] Push pc;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i == 0) {
    // VkDrawIndirectCommand: vertexCount(36), instanceCount, firstVertex, firstInstance
    vk::RawBufferStore<uint4>(pc.args_addr, uint4(36u, pc.count, 0u, 0u));
    // VkDrawMeshTasksIndirectCommandEXT: groupCountX/Y/Z (one group per box)
    vk::RawBufferStore<uint3>(pc.args_addr + 16u, uint3(pc.count, 1u, 1u));
    vk::RawBufferStore<uint>(pc.count_addr, 1u);
    vk::RawBufferStore<uint>(pc.churn_addr, pc.count);  // touch the churned buffer
  }
  if (i >= pc.count) return;

  // A diagonal row of boxes marching front-to-back through the origin so they
  // interleave in depth with rx's cube; each takes a different texture-array
  // layer so the array sampling is visible.
  float t = (float)i - ((float)pc.count - 1.0) * 0.5;
  float3 pos = float3(t * 0.7, 0.5, t * 0.5);
  float phase = (float)i * 0.9 + pc.time;
  float3 col = 0.55 + 0.45 * float3(sin(phase), sin(phase + 2.1), sin(phase + 4.2));
  uint layer = i % pc.layer_count;

  uint64_t base = pc.instance_addr + (uint64_t)i * 48u;
  vk::RawBufferStore<float4>(base, float4(pos, 0.32));
  vk::RawBufferStore<float4>(base + 16u, float4(col, 1.0));
  vk::RawBufferStore<uint>(base + 32u, layer);
}
