// Heightfield fluid surface, vertex stage. Fully procedural: no vertex or index
// buffers. One non-indexed draw of 6*N*N vertices, instance_count = 2. The
// vertex id decodes a grid cell (i,j) and one of the two triangles' corners;
// the instance id selects the fluid (0 = water, 1 = lava). We map the cell to
// world XZ from the solver's params CB, sample bed + state, and lift the vertex
// to that fluid's surface height. Cells whose own depth is below eps collapse to
// a clipped position so dry regions cost nothing downstream.

#include "rhi_bindings.hlsli"

// Frame globals prefix (mesh_pipeline.h FrameGlobals): only the leading matrices
// + jitter are read here, so the struct stops once they are covered.
struct FrameGlobals {
  column_major float4x4 view_proj;
  column_major float4x4 prev_view_proj;
  column_major float4x4 inv_view_proj;
  float2 jitter;
  float2 prev_jitter;
};
[[vk::binding(0, 0)]] ConstantBuffer<FrameGlobals> frame : register(b0, space0);

// Transient fluid set (set 1). state = (dw, dl, T, C); bed = world Y of the
// static bed (terrain + crust base). The VS reads state + bed + params.
[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] Texture2D<float4> state_tex : register(t0, space1);
[[vk::combinedImageSampler]] [[vk::binding(0, 1)]] SamplerState state_samp : register(s0, space1);
[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] Texture2D<float> bed_tex : register(t1, space1);
[[vk::combinedImageSampler]] [[vk::binding(1, 1)]] SamplerState bed_samp : register(s1, space1);
[[vk::combinedImageSampler]] [[vk::binding(2, 1)]] Texture2D<float4> vel_tex : register(t2, space1);
[[vk::combinedImageSampler]] [[vk::binding(2, 1)]] SamplerState vel_samp : register(s2, space1);

// Mirrors fluid_sim.h FluidSim::GpuParams: the world/grid mapping.
struct FluidParams {
  float2 origin;    // world XZ of the min corner
  float extent;     // meters (square domain)
  float texel;      // cell size l (meters)
  float resolution; // solver cells per side (float, for uv math)
  float3 pad;
};
[[vk::binding(3, 1)]] ConstantBuffer<FluidParams> params : register(b3, space1);

// Shading knobs (fluid_surface.cc FluidSurfacePush); the VS only needs eps.
struct FluidSurfacePush {
  float eps;
  float time;
  float water_absorption;
  float foam_scale;
  float4 absorb_color;
  float flow_period;
  float foam_speed_lo;
  float foam_speed_hi;
  float lava_emissive;
  uint grid;  // cells per side of this draw (matches the CPU vertex count)
  uint3 pad;
};
PUSH_CONSTANTS(FluidSurfacePush, push);

struct VsOut {
  float4 sv_position : SV_Position;
  [[vk::location(0)]] float3 world_pos : TEXCOORD0;
  [[vk::location(1)]] float2 uv : TEXCOORD1;        // 0..1 over the domain
  [[vk::location(2)]] float depth : TEXCOORD2;      // this fluid's depth at the vertex (m)
  [[vk::location(3)]] float4 curr_clip : TEXCOORD3;
  [[vk::location(4)]] float4 prev_clip : TEXCOORD4;
  [[vk::location(5)]] nointerpolation uint fluid : TEXCOORD5;  // 0 water, 1 lava
};

// Two-triangle quad corners for the six vertices of a cell.
static const float2 kCorners[6] = {
    float2(0, 0), float2(1, 0), float2(1, 1),
    float2(0, 0), float2(1, 1), float2(0, 1),
};

VsOut main(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  // The render grid may be coarser than the solver texture; the CPU pushes the
  // exact cells-per-side of this draw so the cell decode always matches the
  // issued vertex count.
  uint grid = max(push.grid, 1u);

  uint cell = vid / 6u;
  uint corner = vid % 6u;
  uint i = cell % grid;
  uint j = cell / grid;
  float2 node = float2((float)i, (float)j) + kCorners[corner];
  float2 uv = node / (float)grid;  // 0..1, clamped sampler handles the far edge

  float2 world_xz = params.origin + uv * params.extent;

  float4 st = state_tex.SampleLevel(state_samp, uv, 0.0);  // dw, dl, T, C
  float bed = bed_tex.SampleLevel(bed_samp, uv, 0.0);
  float crust = st.w;
  float dl = st.y;
  float dw = st.x;

  // Lava rides on bed + crust; water rides on top of the lava column.
  float surf;
  float depth;
  if (iid == 1u) {
    surf = bed + crust + dl;
    depth = dl;
  } else {
    surf = bed + crust + dl + dw;
    depth = dw;
  }

  VsOut o;
  o.fluid = iid;
  o.uv = uv;
  o.depth = depth;
  if (depth < push.eps) {
    // Dry vertex for this fluid: emit a degenerate position. Note this kills
    // any triangle with a mixed wet/dry corner set, so the rendered surface
    // retreats up to one cell from the true wetting front — the PS depth fade
    // hides the resulting edge.
    o.sv_position = float4(0, 0, 0, 0);
    o.world_pos = float3(world_xz.x, surf, world_xz.y);
    o.curr_clip = float4(0, 0, 0, 1);
    o.prev_clip = float4(0, 0, 0, 1);
    return o;
  }

  float4 world = float4(world_xz.x, surf, world_xz.y, 1.0);
  float4 clip = mul(frame.view_proj, world);
  o.world_pos = world.xyz;
  o.curr_clip = clip;
  // The XZ footprint is static; reproject the same world point (the small
  // per-frame height change is a fraction of a pixel of parallax) so the motion
  // vector carries camera motion, like the water surface.
  o.prev_clip = mul(frame.prev_view_proj, world);
  // Rasterise with the frame's temporal jitter (jitter never touches the
  // reprojected curr/prev clip), matching every other geometry pass.
  o.sv_position = clip + float4(frame.jitter * clip.w, 0.0, 0.0);
  return o;
}
