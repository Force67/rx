// RCGI cache shade, software variant: reads surface colour the SDF probe trace
// packed into the entry and casts the sun-shadow trace against the SDF clipmap,
// so the pipeline creates and runs on devices without ray query (RX_RCGI_SW /
// --no-rt). Zero RayQuery code reaches the SPIR-V. The shared body lives in
// rcgi_cache_shade_body.hlsli; RCGI_TRACE_SDF selects the SDF trace seam and the
// SDF clipmap bindings (set 1).
#define RCGI_TRACE_SDF 1
#include "gi/rcgi_cache_shade_body.hlsli"
