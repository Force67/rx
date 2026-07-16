// Shared by the volumetric precipitation shaders (precip_volume.vs/.ps,
// precip_splash.vs/.ps): one push block for all of them, the stateless
// per-instance hashes, and the sky-occlusion map decode. Positions are pure
// functions of (instance id, time, weather) - no sim buffers anywhere.
#ifndef RX_PRECIP_COMMON_HLSLI_
#define RX_PRECIP_COMMON_HLSLI_

struct PrecipPush {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  float3 cam_right; float time;       // seconds
  float3 cam_up;    float intensity;  // precipitation 0..1
  float3 cam_pos;   uint flags;       // 1 snow, 2 froxel volume valid
  float3 sun_dir;   float sun_intensity;  // travel direction
  float3 sun_color; float ambient;
  float4 wind;      // xy wind velocity on xz (m/s), z gustiness, w lightning
  float4 occl;      // x center_x, y center_z, z 1/half_extent, w top_y
  float2 jitter;    // ndc units, same as the frame's geometry
  float dt;         // last frame delta, rewinds the motion vector
  float occl_range; // sky-occlusion y range (top_y - range = map bottom)
};

static const float2 kQuadCorners[4] = {float2(-1, -1), float2(1, -1),
                                       float2(-1, 1), float2(1, 1)};
static const float kTau = 6.28318530718;

uint PcgHash(uint v) {
  v = v * 747796405u + 2891336453u;
  v = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
  return (v >> 22u) ^ v;
}

// Four decorrelated 0..1 floats from one instance id.
float4 Hash4(uint id) {
  uint a = PcgHash(id);
  uint b = PcgHash(a);
  uint c = PcgHash(b);
  uint d = PcgHash(c);
  return float4(a, b, c, d) * (1.0 / 4294967295.0);
}

// Wraps a world-anchored coordinate into a window following the camera: the
// value is fixed in world space, only the visible window slides.
float WrapWindow(float u, float lo, float size) {
  return u - floor((u - lo) / size) * size;
}

// Sky-occlusion decode (see PrecipOcclusion in precip_occlusion.h): the
// sampled top-down depth turns back into the occluder's world height.
float2 OcclusionUv(float2 world_xz, float4 occl) {
  return (world_xz - occl.xy) * occl.z * 0.5 + 0.5;
}
float OccluderHeight(float depth_sample, float4 occl, float occl_range) {
  return occl.w - depth_sample * occl_range;
}

#endif  // RX_PRECIP_COMMON_HLSLI_
