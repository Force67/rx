// render2d 2D-lighting pass - fragment stage. Radial falloff from the light
// centre with an optional spot cone, emitted as additive linear radiance into
// the light-accumulation target (which was cleared to the ambient colour).

struct PsIn {
  float4 pos : SV_Position;
  [[vk::location(0)]] float2 world : TEXCOORD0;
  [[vk::location(1)]] nointerpolation float2 center : TEXCOORD1;
  [[vk::location(2)]] nointerpolation float4 color : COLOR0;
  [[vk::location(3)]] nointerpolation float4 params : TEXCOORD2;  // radius, intensity, cone, falloff
  [[vk::location(4)]] nointerpolation float2 dir : TEXCOORD3;
};

float4 main(PsIn i) : SV_Target0 {
  float radius = i.params.x;
  float intensity = i.params.y;
  float cone = i.params.z;
  float falloff = i.params.w;

  float2 delta = i.world - i.center;
  float dist = length(delta) / max(radius, 1e-3);
  if (dist > 1.0) discard;
  float atten = pow(saturate(1.0 - dist), max(falloff, 0.01));

  // Spot cone (cone <= -1 marks an omni light).
  if (cone > -1.0 && dist > 1e-4) {
    float a = dot(normalize(delta), i.dir);
    atten *= smoothstep(cone, min(cone + 0.15, 1.0), a);
  }

  return float4(i.color.rgb * (intensity * atten), 1.0);
}
