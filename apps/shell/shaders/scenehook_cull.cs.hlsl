// Scene-hook demo: a compute "placement/cull" pass that writes a small buffer-
// device-address arena of per-instance box transforms + colours, later read by
// the draw pass in the same render-graph hook. Proves compute-in-hook running on
// rx's frame command list before the app opens its own raster section.
struct Push {
  column_major float4x4 view_proj;  // shared push layout with the draw pass
  uint64_t addr;                    // device address of the instance arena
  float2 jitter;
  uint count;
  float time;
};
[[vk::push_constant]] Push pc;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  uint i = tid.x;
  if (i >= pc.count) return;
  // A diagonal row of boxes marching front-to-back straight through the origin,
  // where the rx cube sits, so some occlude it and some are occluded by it.
  float t = (float)i - ((float)pc.count - 1.0) * 0.5;
  float3 pos = float3(t * 0.7, 0.5, t * 0.5);
  float phase = (float)i * 0.9 + pc.time;
  float3 col = 0.5 + 0.5 * float3(sin(phase), sin(phase + 2.1), sin(phase + 4.2));
  uint64_t base = pc.addr + (uint64_t)i * 32u;  // float4 pos+size, float4 colour
  vk::RawBufferStore<float4>(base, float4(pos, 0.32));
  vk::RawBufferStore<float4>(base + 16u, float4(col, 1.0));
}
