// Interest-bubble visualizer: one wire sphere per draw, generated entirely
// from SV_VertexID (no vertex buffer). Three great-circle rings (XZ, XY, YZ)
// as a line list: segments*2 vertices per ring.
struct Push {
  column_major float4x4 view_proj;  // rx's exact unjittered view*proj this frame
  float3 center;
  float radius;
  float4 color;
  float2 jitter;  // rx TAA jitter, ndc units
  uint segments;
  float pad;
};
[[vk::push_constant]] Push pc;

struct VsOut {
  float4 pos : SV_Position;
  float4 color : COLOR0;
};

VsOut main(uint vid : SV_VertexID) {
  const uint per_ring = pc.segments * 2u;
  const uint ring = vid / per_ring;
  const uint k = vid % per_ring;
  // Line list over the ring: segment i spans points i and i+1 (mod wraps the
  // last segment back to point 0 via the angle period).
  const uint point_idx = (k >> 1u) + (k & 1u);
  const float angle = (6.2831853f * point_idx) / pc.segments;
  const float c = cos(angle);
  const float s = sin(angle);
  float3 p;
  if (ring == 0u) p = float3(c, 0, s);       // equator (ground plane)
  else if (ring == 1u) p = float3(c, s, 0);  // XY meridian
  else p = float3(0, c, s);                  // YZ meridian
  const float3 world = pc.center + p * pc.radius;
  float4 clip = mul(pc.view_proj, float4(world, 1.0));
  clip.xy += pc.jitter * clip.w;  // stay pixel-aligned with rx geometry under TAA
  VsOut o;
  o.pos = clip;
  o.color = pc.color;
  return o;
}
