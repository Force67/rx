// Scene-hook demo draw pass fragment stage: shades the boxes into rx's HDR-
// linear scene colour and writes the R32F depth-export copy so rx's depth-aware
// passes (sky/fog/aerial/SSAO) respect the app geometry.
struct VsOut {
  float4 pos : SV_Position;
  float3 color : COLOR0;
  float3 normal : NORMAL0;
};

struct PsOut {
  float4 scene : SV_Target0;  // RGBA16F scene colour (pre-tonemap, linear)
  float depth : SV_Target1;   // R32F depth-export copy (reversed-z device depth)
};

PsOut main(VsOut i) {
  float3 n = normalize(i.normal);
  float3 l = normalize(float3(0.4, 0.85, 0.3));
  float ndl = saturate(dot(n, l)) * 0.85 + 0.15;
  PsOut o;
  o.scene = float4(i.color * ndl, 1.0);
  o.depth = i.pos.z;  // matches rx's reversed-z depth_export
  return o;
}
