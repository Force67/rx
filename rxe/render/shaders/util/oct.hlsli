#ifndef RX_UTIL_OCT_HLSLI_
#define RX_UTIL_OCT_HLSLI_

// Octahedral encode/decode of a unit direction onto the [-1,1]^2 square. The
// proven math from the DDGI probe path (ddgi_rays.cs.hlsl); shared here so new
// GI passes reuse one copy. The existing inline copies in ddgi/mesh.ps are left
// untouched on purpose.

float2 RxOctEncode(float3 d) {
  d /= (abs(d.x) + abs(d.y) + abs(d.z));
  float2 o = d.xz;
  if (d.y < 0.0) o = (1.0 - abs(d.zx)) * float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
  return o;
}

float3 RxOctDecode(float2 o) {
  float3 d = float3(o.x, 1.0 - abs(o.x) - abs(o.y), o.y);
  if (d.y < 0.0) {
    float2 sign_xz = float2(d.x >= 0.0 ? 1.0 : -1.0, d.z >= 0.0 ? 1.0 : -1.0);
    d.xz = (1.0 - abs(d.zx)) * sign_xz;
  }
  return normalize(d);
}

#endif  // RX_UTIL_OCT_HLSLI_
