// Offscreen RHI test: a single triangle covering the image centre, generated
// from SV_VertexID (no vertex buffer). NDC y is down in Vulkan clip space, but
// the triangle is symmetric about the centre so the exact orientation does not
// matter - it covers (0,0) and leaves the image corners uncovered.
//
// Written in slang (the hlsl twin lives in git history): the pair doubles as
// the acceptance test for the slang half of rx_embed_shaders, spirv and dxil
// sidecar both.
static const float2 kPos[3] = {
    float2(-0.5, 0.5),
    float2(0.5, 0.5),
    float2(0.0, -0.5),
};

[shader("vertex")]
float4 main(uint vid : SV_VertexID) : SV_Position {
  return float4(kPos[vid], 0.0, 1.0);
}
