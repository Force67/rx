#include "rhi_bindings.hlsli"
// Imposter billboard vertex: one instanced quad per distant tree, expanded
// around the instance origin facing the camera (cylindrical: yaw only, up
// stays world-up so trunks stay vertical), with the hemi-octahedral cell of
// the instance->camera direction selected per instance.
struct Instance {
  float3 position;
  float scale;
};
[[vk::binding(0, 0)]] StructuredBuffer<Instance> instances : register(t0, space0);

struct DrawPush {
  column_major float4x4 view_proj;
  float4 camera;     // xyz eye
  float4 sun;        // xyz travel, w intensity
  float4 sun_color;  // rgb, w ambient
  float radius;      // baked mesh bounding radius
  float center_y;    // baked mesh center height
  float grid;        // octahedral cells per axis
  float pad0;
};
PUSH_CONSTANTS(DrawPush, pc);

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;      // atlas uv
  [[vk::location(1)]] float3 world_pos : POSITION1;
};

// Maps an upper-hemisphere direction to [0,1]^2 (hemi-octahedral).
float2 HemiOctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  return float2(d.x + d.z, d.z - d.x) * 0.5 + 0.5;
}

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  Instance inst = instances[iid];
  float r = pc.radius * inst.scale;
  float3 center = inst.position + float3(0.0, pc.center_y * inst.scale, 0.0);

  // View direction from the instance to the camera, clamped to the upper
  // hemisphere the bake covered.
  float3 to_cam = pc.camera.xyz - center;
  float3 dir = normalize(float3(to_cam.x, max(to_cam.y, 0.0), to_cam.z) + 1e-5);
  float2 cell_uv = HemiOctEncode(dir);
  float2 cell = min(floor(cell_uv * pc.grid), pc.grid - 1.0);

  // Cylindrical billboard basis.
  float3 fwd = normalize(float3(to_cam.x, 0.0, to_cam.z) + 1e-5);
  float3 right = float3(fwd.z, 0.0, -fwd.x);
  float2 corner = float2((vid & 1) != 0 ? 1.0 : -1.0, (vid & 2) != 0 ? 1.0 : -1.0);
  float3 world = center + right * (corner.x * r) + float3(0.0, corner.y * r, 0.0);

  VsOut o;
  o.pos = mul(pc.view_proj, float4(world, 1.0));
  // The bake's ortho has +y up in clip space with no vulkan y-flip, so the
  // mesh top lands at v = 1; u mirrors because the baked view looks BACK
  // along the view direction.
  float2 quad_uv = float2(0.5 - corner.x * 0.5, 0.5 + corner.y * 0.5);
  o.uv = (cell + quad_uv) / pc.grid;
  o.world_pos = world;
  return o;
}
