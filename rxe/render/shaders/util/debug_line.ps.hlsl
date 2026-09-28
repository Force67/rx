// Editor debug lines: emit the interpolated per-vertex colour unchanged.
struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float4 color : COLOR;
};

float4 main(PsIn input) : SV_Target0 {
  return input.color;
}
