// Ray-query variant of precip_volume.vs: one inline sun-visibility ray per
// vertex shadows the drops/flakes through the scene TLAS. Built only when the
// device has ray query; the plain precip_volume.vs is the fallback.
#define RX_PRECIP_RT 1
#include "precip_volume.vs.hlsl"
