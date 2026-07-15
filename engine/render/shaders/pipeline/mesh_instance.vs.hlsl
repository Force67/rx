// Hardware-instanced permutation of mesh.vs.hlsl. Persistent current and
// previous matrix streams emit object motion for group updates; unchanged and
// newly appended instances receive the current matrix in both streams.
#define RX_INSTANCED 1
#include "mesh.vs.hlsl"
