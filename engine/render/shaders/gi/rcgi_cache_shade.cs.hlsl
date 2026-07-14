// RCGI cache shade, hardware variant: re-resolves the hit triangle/material
// through the bindless scene tables and casts a TLAS sun-shadow ray. The shared
// body (including the software SDF variant) lives in rcgi_cache_shade_body.hlsli;
// this entry selects the ray-query path by leaving RCGI_TRACE_SDF undefined. See
// rcgi_cache_shade_sw.cs.hlsl for the software (SDF clipmap) variant.
#include "gi/rcgi_cache_shade_body.hlsli"
