#include "procedural_grass.hlsli"

#include "gi/material_class.hlsli"

float2 OctEncode(float3 direction) {
  direction /= abs(direction.x) + abs(direction.y) + abs(direction.z);
  float2 encoded = direction.xz;
  if (direction.y < 0.0) {
    encoded = (1.0 - abs(direction.zx)) *
              float2(direction.x >= 0.0 ? 1.0 : -1.0,
                     direction.z >= 0.0 ? 1.0 : -1.0);
  }
  return encoded;
}

struct PsOut {
  float4 normal : SV_Target0;
  float2 motion : SV_Target1;
  float depth : SV_Target2;
};

PsOut main(GrassVsOut input) {
  GrassTypeData type = LoadGrassType(input.type);
  PsOut output;
  // Unresolved distant blade normals represent a distribution, not a mirror:
  // blend toward the broad growth normal and widen the lobe with distance.
  float3 stable_normal = normalize(lerp(input.normal, float3(0.0, 1.0, 0.0),
                                        input.lod * 0.55));
  float roughness = lerp(type.material.x, 1.0, input.lod * 0.7);
  output.normal = float4(OctEncode(stable_normal), clamp(roughness, 0.045, 1.0),
                         kMatClassVegetation);
  float2 current = input.curr_clip.xy / input.curr_clip.w;
  float2 previous = input.prev_clip.xy / input.prev_clip.w;
  output.motion = (previous - current) * 0.5;
  output.depth = input.sv_position.z;
  return output;
}
