// Ray-query variant of froxel_scatter.cs: the sun term is shadowed by one
// inline ray per froxel instead of the cascade atlas. Built only when the
// device has ray query; the plain froxel_scatter.cs is the fallback.
#define RX_FROXEL_RT 1
#include "froxel_scatter.cs.hlsl"
