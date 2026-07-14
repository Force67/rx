// RCGI probe trace, hardware variant: TLAS ray-query visibility. The shared body
// (including the software SDF variant) lives in rcgi_probe_trace_body.hlsli;
// this entry selects the ray-query path by leaving RCGI_TRACE_SDF undefined. See
// rcgi_probe_trace_sw.cs.hlsl for the software (SDF clipmap) variant.
#include "gi/rcgi_probe_trace_body.hlsli"
