// render2d lit-composite - fullscreen triangle (no vertex buffer). Emits a
// single oversized triangle covering the viewport; the fragment stage samples
// the albedo and light targets at the interpolated uv.

struct VsOut {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VsOut main(uint vid : SV_VertexID) {
  VsOut o;
  o.uv = float2((vid << 1) & 2, vid & 2);
  o.pos = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}
