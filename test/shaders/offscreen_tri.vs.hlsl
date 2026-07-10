// Offscreen RHI test: a single triangle covering the image centre, generated
// from SV_VertexID (no vertex buffer). NDC y is down in Vulkan clip space, but
// the triangle is symmetric about the centre so the exact orientation does not
// matter - it covers (0,0) and leaves the image corners uncovered.
static const float2 kPos[3] = {
    float2(-0.5, 0.5),
    float2(0.5, 0.5),
    float2(0.0, -0.5),
};

float4 main(uint vid : SV_VertexID) : SV_Position {
  return float4(kPos[vid], 0.0, 1.0);
}
