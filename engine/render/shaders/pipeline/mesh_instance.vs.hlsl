// Hardware-instanced permutation of mesh.vs.hlsl. One persistent vertex stream
// carries a model matrix per streamed prop; static instances reuse it as the
// previous transform, so they produce zero motion until promoted to an entity.
#define RX_INSTANCED 1
#include "mesh.vs.hlsl"
