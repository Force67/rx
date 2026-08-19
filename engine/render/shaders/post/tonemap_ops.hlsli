#ifndef RX_TONEMAP_OPS_HLSLI_
#define RX_TONEMAP_OPS_HLSLI_

// The display transform, extracted so anything that needs to compare against
// the FRAME's look applies the frame's actual curve rather than a lookalike.
// The reference-comparison pass (post/reference_compare.cs.hlsl) is the reason
// this is a header: a display-referred difference against a different tonemap
// is a measurement of the tonemap, not of the renderer.
//
// Keep the operator ids in step with render::TonemapOperator (core/settings.h).

// Narkowicz ACES fit. Cheap, no LUT, good enough until a proper grading
// stage with white balance lands.
float3 TonemapAces(float3 x) {
  return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// AgX (Sobotka), Filament-style polynomial fit. Wide-shoulder log encode with
// an inset gamut: bright saturated light desaturates smoothly toward white
// instead of clipping per channel (the ACES fit turns hot foliage/sky into
// flat white patches and skews hues near clip).
float3 AgxContrast(float3 x) {
  float3 x2 = x * x;
  float3 x4 = x2 * x2;
  return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
         0.1191 * x - 0.00232;
}
float3 TonemapAgx(float3 c) {
  const float3x3 agx_mat = float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                                    0.0784335999999992, 0.878468636469772, 0.0784336,
                                    0.0792237451477643, 0.0791661274605434, 0.879142973793104);
  const float3x3 agx_mat_inv =
      float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
               -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
               -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
  const float min_ev = -12.47393;
  const float max_ev = 4.026069;
  c = mul(c, agx_mat);
  c = clamp(log2(max(c, 1e-10)), min_ev, max_ev);
  c = (c - min_ev) / (max_ev - min_ev);
  c = AgxContrast(c);
  c = mul(c, agx_mat_inv);
  // The fit outputs a 2.2-encoded value; back to linear for the output encode.
  return pow(saturate(c), 2.2);
}


// Dispatch by render::TonemapOperator id. 0 = none (clamp), 1 = ACES, 2 = AgX.
float3 TonemapApply(float3 c, uint op) {
  if (op == 1u) return TonemapAces(c);
  if (op == 2u) return TonemapAgx(c);
  return saturate(c);
}

#endif  // RX_TONEMAP_OPS_HLSLI_
