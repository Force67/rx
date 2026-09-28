// RCGI probe trace, software variant: sphere-traces the global SDF clipmap
// instead of the TLAS, so the pipeline creates and runs on devices without ray
// query (RX_RCGI_SW / --no-rt). Zero RayQuery code reaches the SPIR-V. The
// shared body lives in rcgi_probe_trace_body.hlsli; RCGI_TRACE_SDF selects the
// SDF trace seam and the SDF clipmap bindings (set 1).
#define RCGI_TRACE_SDF 1
#include "gi/rcgi_probe_trace_body.hlsli"
