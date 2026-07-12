// Interest-bubble visualizer: flat translucent line color.
struct PsIn {
  float4 pos : SV_Position;
  float4 color : COLOR0;
};

float4 main(PsIn i) : SV_Target0 {
  return i.color;
}
