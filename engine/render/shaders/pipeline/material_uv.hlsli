#ifndef RX_MATERIAL_UV_HLSLI_
#define RX_MATERIAL_UV_HLSLI_

// The one definition of a material's animated uv scroll (waterfalls, rivers,
// lava). Include after `frame` (set 0 b0) and `material` (set 1 b0) are
// declared and run the interpolated uv through it before anything samples a
// material map.
//
// The prepass and the main pass have to alpha-test the same texel. When each
// one spelled the scroll out itself only the main pass got it, so a masked
// scrolling material discarded a different set of fragments in each pass and
// tore holes into depth and motion. Keep the formula here, never inline in a
// pass, so a pass that samples the material cannot drift out of phase with the
// ones that already do.
float2 MaterialUv(float2 uv) { return uv + frame.time * material.uv_scroll; }

#endif  // RX_MATERIAL_UV_HLSLI_
